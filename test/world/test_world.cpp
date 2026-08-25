#include <gtest/gtest.h>

#include <array>
#include <memory>

#include "../../src/module/module.h"
#include "../../src/settings.h"
#include "../../src/world/world.h"

namespace {

	using Blocks = RfidPayload;

	// Two modules joined left port to right port: [2] --left--> ... <--right-- [1].
	class WorldTest : public ::testing::Test {
	protected:
		world::State world_;

		static constexpr uint8_t FIRST  = 1;
		static constexpr uint8_t SECOND = 2;

		// The distance from a module's junction to any of its endpoints, in micrometres.
		static constexpr int64_t LEG_UM = 180000;

		// The belt speed every module in this fixture runs at, in micrometres per second.
		static constexpr double LEG_SPEED_UM_PER_S = 100000.0;

		// A pinned linear belt: same speed both ways, so the arithmetic below stays readable.
		static world::Calibration linear_calibration() {
			world::Calibration calibration = world::Calibration::nominal();
			for (std::size_t index = 0; index < world::BELT_CURVE_POINTS; index++) {
				const double speed = world::BELT_CURVE_COMMANDS[index] / 100.0 * LEG_SPEED_UM_PER_S;
				calibration.mechanical.big_belt.left_um_per_s[index]    = speed;
				calibration.mechanical.big_belt.right_um_per_s[index]   = speed;
				calibration.mechanical.small_belt.left_um_per_s[index]  = speed;
				calibration.mechanical.small_belt.right_um_per_s[index] = speed;
			}
			return calibration;
		}

		void SetUp() override {
			world::set_calibration_override(linear_calibration());
			world_.clear();
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}

			settings::ModuleSettings first{};
			first.active = true;
			first.left   = module::ModuleId{SECOND};
			settings::set_module_settings(module::ModuleId{FIRST}, first);

			settings::ModuleSettings second{};
			second.active = true;
			second.right  = module::ModuleId{FIRST};
			settings::set_module_settings(module::ModuleId{SECOND}, second);

			for (const uint8_t id : {FIRST, SECOND}) {
				world_.add_module(module::ModuleId{id});
			}
		}

		void TearDown() override {
			world::clear_calibration_seed();
			world_.clear();
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
		}

		module::Module* hardware(uint8_t id) { return world_.module(module::ModuleId{id}); }

		void place(uint8_t tub_id, uint8_t module_id, world::Position position) {
			world_.place(tub_id, module::ModuleId{module_id}, position, Blocks{});
		}

		void advance(int ticks) {
			for (int index = 0; index < ticks; index++) {
				world_.advance_tick();
			}
		}

		int64_t position_um(uint8_t tub_id) { return world_.tub(tub_id)->position_um; }
	};

} // namespace

TEST_F(WorldTest, BeltStepAccumulatesFractionalTravelInsteadOfLosingIt) {
	// At one micrometre a tick, a discarded remainder would round the travel away to nothing.
	const world::BeltCurve& curve = linear_calibration().mechanical.big_belt;

	int64_t carry     = 0;
	int64_t travelled = 0;
	for (int tick = 0; tick < 1000; tick++) {
		travelled += world::belt_step_um(1.0, curve, carry);
	}
	EXPECT_EQ(travelled, 1000);

	// A tenth of a percent is below one micrometre per tick and still accumulates exactly.
	carry     = 0;
	travelled = 0;
	for (int tick = 0; tick < 1000; tick++) {
		travelled += world::belt_step_um(0.1, curve, carry);
	}
	EXPECT_EQ(travelled, 100);
}

TEST_F(WorldTest, BeltStepIsSignedAndSymmetric) {
	const world::BeltCurve& curve = linear_calibration().mechanical.big_belt;

	int64_t forward_carry  = 0;
	int64_t backward_carry = 0;
	EXPECT_EQ(world::belt_step_um(100.0, curve, forward_carry), 100);
	EXPECT_EQ(world::belt_step_um(-100.0, curve, backward_carry), -100);

	int64_t stopped_carry = 5;
	EXPECT_EQ(world::belt_step_um(0.0, curve, stopped_carry), 0);
	EXPECT_EQ(stopped_carry, 0);
}

TEST_F(WorldTest, BeltStepIsSymmetricOnlyBecauseThisFixturePinnedItThatWay) {
	// The symmetry above is a property of the pinned belt, not of the model.
	const world::Calibration measured = world::calibration_for(module::ModuleId{5}, 1);

	const double leftward  = world::belt_speed_um_per_s(measured.mechanical.big_belt, 100.0);
	const double rightward = world::belt_speed_um_per_s(measured.mechanical.big_belt, -100.0);

	EXPECT_GT(leftward, 0.0);
	EXPECT_LT(rightward, 0.0);
	EXPECT_NE(leftward, -rightward);
}

TEST_F(WorldTest, ATubOnAStoppedBeltDoesNotMove) {
	place(1, FIRST, world::Position::CENTER);
	const int64_t started = position_um(1);

	advance(100);

	EXPECT_EQ(position_um(1), started);
}

TEST_F(WorldTest, TheServoDeadZoneHoldsATubOnTheJunction) {
	place(1, FIRST, world::Position::CENTER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{90});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});

	advance(100);

	EXPECT_EQ(position_um(1), LEG_UM);
	EXPECT_EQ(world_.position_of(1).value(), world::Position::CENTER);
}

TEST_F(WorldTest, TheBigBeltSignDecidesWhichEndpointATubHeadsFor) {
	place(1, FIRST, world::Position::CENTER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{0});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});

	// 100 mm/s for 100 ticks is 10 mm of travel towards the left endpoint.
	advance(100);
	EXPECT_EQ(position_um(1), LEG_UM + 10000);

	// The big belt is one axis rather than two opposing arms.
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100});
	advance(100);
	EXPECT_EQ(position_um(1), LEG_UM);
}

TEST_F(WorldTest, AnOutwardSmallBeltCannotDivertATubWhileTheServoIsNeutral) {
	const int64_t right_laser_um = linear_calibration().hardware.right_laser_position_um;
	place(1, FIRST, world::Position::RIGHT_LASER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{0});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});

	// The first tick that reaches the junction deliberately stops there. On the next tick, a
	// neutral servo should leave the tub on the straight big-belt route; merely running the small
	// belt outward cannot make the routing decision.
	advance(static_cast<int>((LEG_UM - right_laser_um) / 100));
	ASSERT_EQ(position_um(1), LEG_UM);
	ASSERT_EQ(world_.tub(1)->belt, world::Belt::BIG);
	advance(1);

	EXPECT_EQ(world_.tub(1)->belt, world::Belt::BIG);
	EXPECT_GT(position_um(1), LEG_UM);
}

TEST_F(WorldTest, TheServoCanDivertFromTheRightSideOntoTheSmallBelt) {
	const int64_t right_laser_um = linear_calibration().hardware.right_laser_position_um;
	place(1, FIRST, world::Position::RIGHT_LASER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{60});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});

	advance(static_cast<int>((LEG_UM - right_laser_um) / 100));
	ASSERT_EQ(position_um(1), LEG_UM);
	advance(1);

	EXPECT_EQ(world_.tub(1)->belt, world::Belt::SMALL);
	EXPECT_EQ(position_um(1), 100);
	advance(1);
	EXPECT_EQ(world_.tub(1)->belt, world::Belt::SMALL);
	EXPECT_EQ(position_um(1), 200);
}

TEST_F(WorldTest, TheServoCanDivertFromTheLeftSideOntoTheSmallBelt) {
	const int64_t left_laser_um = linear_calibration().hardware.left_laser_position_um;
	place(1, FIRST, world::Position::LEFT_LASER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{120});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100});
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});

	advance(static_cast<int>((left_laser_um - LEG_UM) / 100));
	ASSERT_EQ(position_um(1), LEG_UM);
	advance(1);

	EXPECT_EQ(world_.tub(1)->belt, world::Belt::SMALL);
	EXPECT_EQ(position_um(1), 100);
}

TEST_F(WorldTest, TheEffectiveNeutralBandsAlwaysPassStraightThrough) {
	for (const double angle : {0.0, 10.0, 20.0, 160.0, 170.0, 180.0}) {
		world_.clear();
		place(1, FIRST, world::Position::CENTER);
		hardware(FIRST)->set_servo_angle(module::ServoAngle{angle});
		hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
		hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});
		advance(1);
		EXPECT_EQ(world_.tub(1)->belt, world::Belt::BIG) << angle;
		EXPECT_GT(position_um(1), LEG_UM) << angle;

		world_.clear();
		place(1, FIRST, world::Position::CENTER);
		hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100});
		advance(1);
		EXPECT_EQ(world_.tub(1)->belt, world::Belt::BIG) << angle;
		EXPECT_LT(position_um(1), LEG_UM) << angle;
	}
}

TEST_F(WorldTest, AnOppositeDiversionAngleBlocksRatherThanPassingTheTub) {
	place(1, FIRST, world::Position::CENTER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{120});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});
	advance(100);
	EXPECT_EQ(world_.tub(1)->belt, world::Belt::BIG);
	EXPECT_EQ(position_um(1), LEG_UM);

	world_.clear();
	place(1, FIRST, world::Position::CENTER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{60});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100});
	advance(100);
	EXPECT_EQ(world_.tub(1)->belt, world::Belt::BIG);
	EXPECT_EQ(position_um(1), LEG_UM);
}

TEST_F(WorldTest, CanonicalDiversionAndBlockBandsIncludeTheirEndpoints) {
	for (const double angle : {50.0, 70.0}) {
		world_.clear();
		place(1, FIRST, world::Position::CENTER);
		hardware(FIRST)->set_servo_angle(module::ServoAngle{angle});
		hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
		hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});
		advance(1);
		EXPECT_EQ(world_.tub(1)->belt, world::Belt::SMALL) << angle;
	}

	for (const double angle : {110.0, 130.0}) {
		world_.clear();
		place(1, FIRST, world::Position::CENTER);
		hardware(FIRST)->set_servo_angle(module::ServoAngle{angle});
		hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100});
		hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});
		advance(1);
		EXPECT_EQ(world_.tub(1)->belt, world::Belt::SMALL) << angle;
	}

	for (const double angle : {80.0, 100.0}) {
		world_.clear();
		place(1, FIRST, world::Position::CENTER);
		hardware(FIRST)->set_servo_angle(module::ServoAngle{angle});
		hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
		hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});
		advance(1);
		EXPECT_EQ(world_.tub(1)->belt, world::Belt::BIG) << angle;
		EXPECT_EQ(position_um(1), LEG_UM) << angle;
	}
}

TEST_F(WorldTest, ASelectedDiversionHoldsUntilTheSmallBeltCanReceiveTheTub) {
	place(1, FIRST, world::Position::CENTER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{60});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{0});
	advance(100);
	EXPECT_EQ(world_.tub(1)->belt, world::Belt::BIG);
	EXPECT_EQ(position_um(1), LEG_UM);

	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});
	advance(1);
	EXPECT_EQ(world_.tub(1)->belt, world::Belt::SMALL);
	EXPECT_EQ(position_um(1), 100);
}

TEST_F(WorldTest, AnInboundSmallBeltTransfersOntoTheBigBeltForEveryNonBlockingRoute) {
	for (const double angle : {0.0, 60.0, 120.0, 180.0}) {
		for (const double big_speed : {-100.0, 0.0, 100.0}) {
			world_.clear();
			place(1, FIRST, world::Position::ENTRY);
			hardware(FIRST)->set_servo_angle(module::ServoAngle{angle});
			hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{-100.0});
			hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{big_speed});

			while (position_um(1) > 0) advance(1);
			ASSERT_EQ(world_.tub(1)->belt, world::Belt::SMALL) << angle << ", " << big_speed;
			ASSERT_EQ(position_um(1), 0) << angle << ", " << big_speed;
			advance(1);

			EXPECT_EQ(world_.tub(1)->belt, world::Belt::BIG) << angle << ", " << big_speed;
			EXPECT_EQ(position_um(1), LEG_UM + static_cast<int64_t>(big_speed))
				<< angle << ", " << big_speed;
		}
	}
}

TEST_F(WorldTest, TheBlockingServoHoldsATubArrivingFromTheSmallBelt) {
	place(1, FIRST, world::Position::ENTRY);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{90.0});
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{-100.0});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100.0});

	while (position_um(1) > 0) advance(1);
	ASSERT_EQ(world_.tub(1)->belt, world::Belt::SMALL);
	ASSERT_EQ(position_um(1), 0);
	advance(100);

	EXPECT_EQ(world_.tub(1)->belt, world::Belt::SMALL);
	EXPECT_EQ(position_um(1), 0);
}

TEST_F(WorldTest, AStoppedSmallBeltDoesNotTransferATubOntoTheBigBelt) {
	place(1, FIRST, world::Position::ENTRY);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{0.0});
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{-100.0});

	while (position_um(1) > 0) advance(1);
	ASSERT_EQ(world_.tub(1)->belt, world::Belt::SMALL);
	ASSERT_EQ(position_um(1), 0);

	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{0.0});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100.0});
	advance(100);

	EXPECT_EQ(world_.tub(1)->belt, world::Belt::SMALL);
	EXPECT_EQ(position_um(1), 0);
}

TEST_F(WorldTest, TransitionalServoAnglesUseSeededSmootherstepAndRemainStablePerEncounter) {
	const auto outcome_at = [this](uint64_t seed, double angle, int held_ticks) {
		world::set_calibration_seed(seed);
		world::set_calibration_override(linear_calibration());
		world_.clear();
		place(1, FIRST, world::Position::CENTER);
		hardware(FIRST)->set_servo_angle(module::ServoAngle{angle});
		hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
		hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{100});
		advance(held_ticks);
		return std::make_pair(world_.tub(1)->belt, position_um(1));
	};

	// Repeating one seeded encounter produces the same binary decision.
	EXPECT_EQ(outcome_at(1234, 35.0, 1), outcome_at(1234, 35.0, 1));

	int near_straight_diversions = 0;
	int midpoint_diversions      = 0;
	int near_divert_diversions   = 0;
	for (uint64_t seed = 0; seed < 1000; seed++) {
		near_straight_diversions += outcome_at(seed, 21.0, 1).first == world::Belt::SMALL ? 1 : 0;
		midpoint_diversions += outcome_at(seed, 35.0, 1).first == world::Belt::SMALL ? 1 : 0;
		near_divert_diversions += outcome_at(seed, 49.0, 1).first == world::Belt::SMALL ? 1 : 0;
	}

	// Quintic smootherstep is almost flat near either endpoint and balanced at the midpoint.
	EXPECT_LT(near_straight_diversions, 25);
	EXPECT_GT(midpoint_diversions, 400);
	EXPECT_LT(midpoint_diversions, 600);
	EXPECT_GT(near_divert_diversions, 975);

	// Between the right-diversion and block bands, a sampled block remains a block while the tub
	// waits; it is not rerolled on each simulation tick.
	bool found_blocked_seed = false;
	for (uint64_t seed = 0; seed < 100 && !found_blocked_seed; seed++) {
		const auto first = outcome_at(seed, 75.0, 1);
		if (first.first == world::Belt::BIG && first.second == LEG_UM) {
			const auto later = outcome_at(seed, 75.0, 100);
			EXPECT_EQ(later.first, world::Belt::BIG);
			EXPECT_EQ(later.second, LEG_UM);
			found_blocked_seed = true;
		}
	}
	EXPECT_TRUE(found_blocked_seed);
}

TEST_F(WorldTest, ATubReachingTheJunctionStopsThereForATick) {
	const int64_t left_laser_um = linear_calibration().hardware.left_laser_position_um;
	place(1, FIRST, world::Position::LEFT_LASER);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100});

	advance(static_cast<int>((left_laser_um - LEG_UM) / 100));
	EXPECT_EQ(position_um(1), LEG_UM);

	// It only leaves once the servo allows it, the single place routing is consulted.
	hardware(FIRST)->set_servo_angle(module::ServoAngle{90});
	advance(10);
	EXPECT_EQ(position_um(1), LEG_UM);

	hardware(FIRST)->set_servo_angle(module::ServoAngle{0});
	advance(10);
	EXPECT_LT(position_um(1), LEG_UM);
}

TEST_F(WorldTest, ATubDrivenOffAnEndpointArrivesOnTheNeighbour) {
	place(1, FIRST, world::Position::CENTER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{0});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
	// The receiving module has to be running its belt inward.
	hardware(SECOND)->set_big_belt_speed(module::BeltSpeed{100});

	advance(1800);

	EXPECT_EQ(world_.tub(1)->module.raw(), SECOND);
	// Handoffs still use physical endpoints; the receiving laser is inset from its endpoint.
	EXPECT_EQ(world_.position_of(1).value(), world::Position::CENTER);
}

TEST_F(WorldTest, ATubIsNotHandedToAModuleThatWouldPushItBack) {
	place(1, FIRST, world::Position::CENTER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{0});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
	hardware(SECOND)->set_big_belt_speed(module::BeltSpeed{-100});

	advance(1800);

	EXPECT_EQ(world_.tub(1)->module.raw(), FIRST);
	EXPECT_EQ(position_um(1), 2 * LEG_UM);
}

TEST_F(WorldTest, AnOccupiedEndpointRefusesAnArrival) {
	place(1, FIRST, world::Position::CENTER);
	place(2, SECOND, world::Position::RIGHT_LASER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{0});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
	hardware(SECOND)->set_big_belt_speed(module::BeltSpeed{100});

	advance(1800);

	// The interesting case is the tick where both tubs wanted the endpoint.
	EXPECT_NE(world_.tub(1)->position_um, world_.tub(2)->position_um);
}

TEST_F(WorldTest, ATubBlocksALaserOnlyNearItsEndpoint) {
	hardware(FIRST)->set_left_laser_enabled(true);
	hardware(FIRST)->set_right_laser_enabled(true);

	place(1, FIRST, world::Position::CENTER);
	EXPECT_TRUE(hardware(FIRST)->is_laser_clear(true));
	EXPECT_TRUE(hardware(FIRST)->is_laser_clear(false));

	world_.remove(1);
	place(1, FIRST, world::Position::LEFT_LASER);
	EXPECT_FALSE(hardware(FIRST)->is_laser_clear(true));
	EXPECT_TRUE(hardware(FIRST)->is_laser_clear(false));

	world_.remove(1);
	place(1, FIRST, world::Position::RIGHT_LASER);
	EXPECT_TRUE(hardware(FIRST)->is_laser_clear(true));
	EXPECT_FALSE(hardware(FIRST)->is_laser_clear(false));

	place(2, FIRST, world::Position::CENTER);
	EXPECT_TRUE(hardware(FIRST)->is_laser_clear(true));
	EXPECT_FALSE(hardware(FIRST)->is_laser_clear(false));

	place(3, FIRST, world::Position::LEFT_LASER);
	EXPECT_FALSE(hardware(FIRST)->is_laser_clear(true));
	EXPECT_FALSE(hardware(FIRST)->is_laser_clear(false));

	world_.remove(2);
	EXPECT_FALSE(hardware(FIRST)->is_laser_clear(true));
	EXPECT_FALSE(hardware(FIRST)->is_laser_clear(false));

	world_.remove(1);
	EXPECT_FALSE(hardware(FIRST)->is_laser_clear(true));
	EXPECT_TRUE(hardware(FIRST)->is_laser_clear(false));

	world_.remove(3);
	EXPECT_TRUE(hardware(FIRST)->is_laser_clear(true));
	EXPECT_TRUE(hardware(FIRST)->is_laser_clear(false));
}

TEST_F(WorldTest, ATagIsPresentedOnlyAtTheReader) {
	Blocks blocks{};
	blocks[0] = {9, 9, 9, 9};
	world_.place(3, module::ModuleId{FIRST}, world::Position::ENTRY, RfidUid{3, 2, 1, 0, 1, 2, 3}, blocks);

	EXPECT_TRUE(hardware(FIRST)->is_rfid_active());
	EXPECT_EQ(hardware(FIRST)->rfid_uid()[0], 3);
	EXPECT_EQ(hardware(FIRST)->rfid_page(0)[0], 9);

	// Driving the small belt inward carries the tub away from the reader.
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{-100});
	advance(static_cast<int>(world::rfid_half_length_um(linear_calibration()) / 100 + 1));

	EXPECT_FALSE(hardware(FIRST)->is_rfid_active());
	EXPECT_EQ(hardware(FIRST)->rfid_uid(), RfidUid{});
}

TEST_F(WorldTest, MovementIsDerivedFromTheLiveBeltSpeed) {
	place(1, FIRST, world::Position::CENTER);
	EXPECT_FALSE(world_.movement_of(1).moving);

	hardware(FIRST)->set_servo_angle(module::ServoAngle{0});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100});
	advance(100);

	const world::Movement forward = world_.movement_of(1);
	EXPECT_TRUE(forward.moving);
	EXPECT_EQ(forward.from, world::Position::CENTER);
	EXPECT_EQ(forward.to, world::Position::LEFT_LASER);

	// Direction is read from the belt rather than remembered on the tub.
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100});
	const world::Movement reversed = world_.movement_of(1);
	EXPECT_EQ(reversed.from, world::Position::LEFT_LASER);
	EXPECT_EQ(reversed.to, world::Position::CENTER);
}

TEST_F(WorldTest, AdvancingIsReproducible) {
	const auto run = [this]() {
		world_.clear();
		world_.place(1, module::ModuleId{FIRST}, world::Position::CENTER, Blocks{});
		world_.place(2, module::ModuleId{FIRST}, world::Position::LEFT_LASER, Blocks{});
		hardware(FIRST)->set_servo_angle(module::ServoAngle{0});
		hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{37.5});
		for (int tick = 0; tick < 500; tick++) {
			world_.advance_tick();
		}
		return std::make_pair(world_.tub(1)->position_um, world_.tub(2)->position_um);
	};

	EXPECT_EQ(run(), run());
}

TEST_F(WorldTest, PlacingATwiceOrOnAnUnknownModuleIsRejected) {
	place(1, FIRST, world::Position::CENTER);
	EXPECT_THROW(place(1, FIRST, world::Position::CENTER), std::invalid_argument);
	EXPECT_THROW(place(2, 7, world::Position::CENTER), std::invalid_argument);
}

TEST_F(WorldTest, ATubIntersectingALaserIsAtThatLaser) {
	const world::Calibration calibration = linear_calibration();
	const int64_t            laser_um    = calibration.hardware.right_laser_position_um;
	const int64_t            radius_um   = world::tub_radius_um(calibration);

	place(1, FIRST, world::Position::RIGHT_LASER);

	// Scenario placement parks at the calibrated sensor, not the physical endpoint.
	EXPECT_EQ(position_um(1), laser_um);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100.0});
	advance(1);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{0.0});
	ASSERT_EQ(position_um(1), laser_um + 100);
	ASSERT_LT(100, radius_um);
	EXPECT_EQ(world_.position_of(1).value(), world::Position::RIGHT_LASER);
	EXPECT_TRUE(hardware(FIRST)->is_right_laser_blocked());

	// At the physical endpoint the inset laser is not blocked.
	world_.remove(1);
	place(1, FIRST, world::Position::CENTER);
	hardware(FIRST)->set_servo_angle(module::ServoAngle{0});
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100.0});
	advance(1800);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{0.0});
	EXPECT_EQ(position_um(1), 0);
	EXPECT_EQ(world_.position_of(1).value(), world::Position::CENTER);
	EXPECT_FALSE(hardware(FIRST)->is_right_laser_blocked());

	// The tub leaves the intersection window only once its centre is more than a radius away.
	world_.remove(1);
	place(1, FIRST, world::Position::RIGHT_LASER);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100.0});
	while (position_um(1) <= laser_um + radius_um) {
		advance(1);
	}
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{0.0});
	EXPECT_EQ(world_.position_of(1).value(), world::Position::CENTER);
	EXPECT_FALSE(hardware(FIRST)->is_right_laser_blocked());
}

TEST_F(WorldTest, TheLeftLaserUsesTheSameTubExtent) {
	const world::Calibration calibration = linear_calibration();
	const int64_t            laser_um    = calibration.hardware.left_laser_position_um;
	const int64_t            radius_um   = world::tub_radius_um(calibration);

	place(1, FIRST, world::Position::LEFT_LASER);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100.0});
	advance(1);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{0.0});

	ASSERT_LT(position_um(1), laser_um);
	ASSERT_GT(position_um(1), laser_um - radius_um);
	EXPECT_EQ(world_.position_of(1).value(), world::Position::LEFT_LASER);
}

TEST_F(WorldTest, ATubHeadingForAPositionIsNotYetThere) {
	// The belt pointing at an endpoint is not the tub being at it.
	place(1, FIRST, world::Position::ENTRY);
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{-100.0});
	advance(1800);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{-100.0});
	advance(1);

	ASSERT_EQ(world_.tub(1)->belt, world::Belt::BIG);
	ASSERT_GT(position_um(1), LEG_UM / 2);
	EXPECT_EQ(world_.position_of(1).value(), world::Position::CENTER);

	// It is at the right laser only once its physical extent reaches the inset beam.
	while (world_.position_of(1) != world::Position::RIGHT_LASER) {
		advance(1);
	}
	EXPECT_EQ(world_.position_of(1).value(), world::Position::RIGHT_LASER);
}

TEST_F(WorldTest, TheReaderUsesItsCalibratedFootprint) {
	place(1, FIRST, world::Position::ENTRY);
	EXPECT_EQ(world_.position_of(1).value(), world::Position::ENTRY);

	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{-100.0});
	advance(static_cast<int>(world::rfid_half_length_um(linear_calibration()) / 100 + 1));
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{0.0});
	EXPECT_LT(position_um(1), LEG_UM);
	EXPECT_EQ(world_.position_of(1).value(), world::Position::CENTER);
}

TEST_F(WorldTest, SensorFootprintsUseInclusivePhysicalBoundaries) {
	world::Calibration calibration               = linear_calibration();
	calibration.endpoint_to_junction_um          = 630000;
	calibration.hardware.right_laser_position_um = 100000;
	calibration.hardware.left_laser_position_um  = 530000;
	calibration.hardware.rfid_position_um        = 315000;
	world::set_calibration_override(calibration);

	EXPECT_EQ(world::tub_radius_um(calibration), 100000);
	EXPECT_EQ(world::rfid_half_length_um(calibration), 120000);

	hardware(FIRST)->set_right_laser_enabled(true);
	place(1, FIRST, world::Position::RIGHT_LASER);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{100.0});
	advance(1000);
	EXPECT_TRUE(hardware(FIRST)->is_right_laser_blocked());
	EXPECT_EQ(world_.position_of(1).value(), world::Position::RIGHT_LASER);
	advance(1);
	hardware(FIRST)->set_big_belt_speed(module::BeltSpeed{0.0});
	EXPECT_FALSE(hardware(FIRST)->is_right_laser_blocked());
	EXPECT_EQ(world_.position_of(1).value(), world::Position::CENTER);

	place(2, FIRST, world::Position::ENTRY);
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{-100.0});
	advance(1200);
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{0.0});
	EXPECT_TRUE(hardware(FIRST)->is_rfid_active());
	EXPECT_EQ(world_.position_of(2).value(), world::Position::ENTRY);
	EXPECT_TRUE(world_.write_rfid_page(module::ModuleId{FIRST}, 0, {1, 2, 3, 4}));
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{-100.0});
	advance(1);
	hardware(FIRST)->set_small_belt_speed(module::BeltSpeed{0.0});
	EXPECT_FALSE(hardware(FIRST)->is_rfid_active());
	EXPECT_EQ(world_.position_of(2).value(), world::Position::CENTER);
	EXPECT_FALSE(world_.write_rfid_page(module::ModuleId{FIRST}, 0, {4, 3, 2, 1}));
}

TEST_F(WorldTest, RfidWritesPersistOnTheTubAcrossSensorRefreshes) {
	Blocks blocks{};
	blocks[2] = {9, 8, 7, 6};
	world_.place(7, module::ModuleId{FIRST}, world::Position::ENTRY, blocks);

	const RfidPage replacement{1, 2, 3, 4};
	ASSERT_TRUE(world_.write_rfid_page(module::ModuleId{FIRST}, 2, replacement));
	EXPECT_EQ(world_.tub(7)->payload[2], replacement);

	// A sensor refresh used to overwrite the reader with the tub's stale original contents.
	advance(1);
	EXPECT_EQ(hardware(FIRST)->rfid_page(2), replacement);
	EXPECT_EQ(world_.tub(7)->payload[2], replacement);
}

TEST_F(WorldTest, RfidReadsAndWritesSelectTheSameTubWhenFootprintsOverlap) {
	Blocks first_blocks{};
	first_blocks[0] = {1, 1, 1, 1};
	world_.place(7, module::ModuleId{FIRST}, world::Position::ENTRY, RfidUid{7, 0, 0, 0, 0, 0, 0}, first_blocks);

	Blocks second_blocks{};
	second_blocks[0] = {2, 2, 2, 2};
	world_.place(8, module::ModuleId{FIRST}, world::Position::ENTRY, RfidUid{8, 0, 0, 0, 0, 0, 0}, second_blocks);

	// Reader contention is deterministic: the lowest tub id is both projected and written.
	ASSERT_EQ(hardware(FIRST)->rfid_uid()[0], 7);
	const RfidPage replacement{9, 8, 7, 6};
	ASSERT_TRUE(world_.write_rfid_page(module::ModuleId{FIRST}, 0, replacement));
	advance(1);
	EXPECT_EQ(hardware(FIRST)->rfid_uid()[0], 7);
	EXPECT_EQ(hardware(FIRST)->rfid_page(0), replacement);
	EXPECT_EQ(world_.tub(7)->payload[0], replacement);
	EXPECT_EQ(world_.tub(8)->payload[0], second_blocks[0]);
}

TEST_F(WorldTest, PositionNamesRoundTrip) {
	for (const world::Position position :
		 {world::Position::ENTRY, world::Position::CENTER, world::Position::LEFT_LASER, world::Position::RIGHT_LASER}) {
		EXPECT_EQ(world::position_from_string(world::to_string(position)).value(), position);
	}
	EXPECT_EQ(world::position_from_string("rfid").value(), world::Position::ENTRY);
	EXPECT_FALSE(world::position_from_string("nowhere").has_value());
}
