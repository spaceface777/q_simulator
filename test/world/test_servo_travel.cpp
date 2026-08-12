#include <gtest/gtest.h>

#include <cmath>

#include "../../src/module/module.h"
#include "../../src/settings.h"
#include "../../src/world/world.h"

namespace {

	class ServoTravelTest : public ::testing::Test {
	protected:
		static constexpr uint8_t MODULE = 1;

		// Restated rather than read out of the routing rule, so the world is not its own oracle.
		static constexpr double DEAD_ZONE_LOW  = 70.0;
		static constexpr double DEAD_ZONE_HIGH = 110.0;

		world::State world_;

		void SetUp() override {
			// Pin the nominal module; per-module variation is covered by test_calibration_statistics.
			world::set_calibration_override(world::Calibration::nominal());
			world_.clear();
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
			settings::ModuleSettings active{};
			active.active = true;
			settings::set_module_settings(module::ModuleId{MODULE}, active);
			world_.add_module(module::ModuleId{MODULE});
		}

		void TearDown() override {
			world::clear_calibration_seed();
			world::set_calibration_config(world::CalibrationConfig{});
			world_.clear();
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
		}

		module::Module* hardware() { return world_.module(module::ModuleId{MODULE}); }

		void advance(int ticks) {
			for (int index = 0; index < ticks; index++) {
				world_.advance_tick();
			}
		}
	};

} // namespace

TEST_F(ServoTravelTest, ACommandedAngleIsReportedImmediatelyButReachedLater) {
	// A hobby servo has no position feedback: firmware reads back the angle it last sent.
	world::command_servo(*hardware(), module::ServoAngle{180.0});

	EXPECT_DOUBLE_EQ(hardware()->servo_angle().raw(), 180.0);
	EXPECT_DOUBLE_EQ(hardware()->servo_actual_angle().raw(), 0.0);
	EXPECT_TRUE(hardware()->is_servo_moving());

	advance(250);
	EXPECT_DOUBLE_EQ(hardware()->servo_angle().raw(), 180.0);
	EXPECT_NEAR(hardware()->servo_actual_angle().raw(), 90.0, 1.0);
	EXPECT_TRUE(hardware()->is_servo_moving());

	advance(250);
	EXPECT_DOUBLE_EQ(hardware()->servo_actual_angle().raw(), 180.0);
	EXPECT_FALSE(hardware()->is_servo_moving());
}

TEST_F(ServoTravelTest, TheArmMovesSteadilyAndOnlyTowardsWhereItWasSent) {
	world::command_servo(*hardware(), module::ServoAngle{180.0});

	double previous = hardware()->servo_actual_angle().raw();
	for (int tick = 0; tick < 500; tick++) {
		advance(1);
		const double now = hardware()->servo_actual_angle().raw();
		EXPECT_GE(now, previous);
		EXPECT_LE(now, 180.0);
		previous = now;
	}
	EXPECT_DOUBLE_EQ(previous, 180.0);
}

TEST_F(ServoTravelTest, SettingAnAngleDirectlyPlacesTheArmWithoutTravel) {
	// The setup path places the arm outright, as distinct from commanding it.
	hardware()->set_servo_angle(module::ServoAngle{90.0});

	EXPECT_DOUBLE_EQ(hardware()->servo_actual_angle().raw(), 90.0);
	EXPECT_FALSE(hardware()->is_servo_moving());
}

TEST_F(ServoTravelTest, ACommandArrivingMidMovementRedirectsTheArmFromWhereItIs) {
	// From where the arm currently is, not from where the previous movement started.
	world::command_servo(*hardware(), module::ServoAngle{180.0});
	advance(250);
	const double midway = hardware()->servo_actual_angle().raw();
	ASSERT_NEAR(midway, 90.0, 1.0);

	world::command_servo(*hardware(), module::ServoAngle{0.0});
	EXPECT_NEAR(hardware()->servo_actual_angle().raw(), midway, 1e-9);
	EXPECT_DOUBLE_EQ(hardware()->servo_angle().raw(), 0.0);

	// And the return trip is half a sweep, because it starts from halfway.
	advance(250);
	EXPECT_DOUBLE_EQ(hardware()->servo_actual_angle().raw(), 0.0);
	EXPECT_FALSE(hardware()->is_servo_moving());
}

TEST_F(ServoTravelTest, TheJunctionHoldsATubWhileTheArmCrossesTheDeadZone) {
	// A tub is held for as long as the crossing takes, not merely at the two endpoints.
	hardware()->set_servo_angle(module::ServoAngle{0.0});
	world_.place(1, module::ModuleId{MODULE}, world::Position::CENTER, {});
	hardware()->set_big_belt_speed(module::BeltSpeed{100.0});

	// Sent all the way across. It is out of the dead zone at first, so the tub leaves.
	world::command_servo(*hardware(), module::ServoAngle{180.0});
	advance(50);
	EXPECT_LT(hardware()->servo_actual_angle().raw(), DEAD_ZONE_LOW);
	EXPECT_GT(world_.tub(1)->position_um, world::calibration_for(module::ModuleId{MODULE}).endpoint_to_junction_um);

	// Inside the dead zone the arm is across the junction, whatever it was told to do next.
	advance(175);
	EXPECT_GE(hardware()->servo_actual_angle().raw(), DEAD_ZONE_LOW);
	EXPECT_LE(hardware()->servo_actual_angle().raw(), DEAD_ZONE_HIGH);

	world_.remove(1);
	world_.place(2, module::ModuleId{MODULE}, world::Position::CENTER, {});
	const int64_t parked = world_.tub(2)->position_um;
	advance(25);
	EXPECT_EQ(world_.tub(2)->position_um, parked) << "the arm was across the junction and let a tub past";
}

TEST_F(ServoTravelTest, ATravellingServoDrawsCurrent) {
	// A servo draws both while travelling and while holding away from rest.
	const world::Calibration calibration = world::calibration_for(module::ModuleId{MODULE});
	const double             idle        = world::electrics_of(*hardware(), calibration).current_a;

	world::command_servo(*hardware(), module::ServoAngle{180.0});
	EXPECT_GT(world::electrics_of(*hardware(), calibration).current_a, idle);

	advance(500);
	ASSERT_FALSE(hardware()->is_servo_moving());
	// Still off its rest angle, so still holding.
	EXPECT_GT(world::electrics_of(*hardware(), calibration).current_a, idle);

	world::command_servo(*hardware(), module::ServoAngle{0.0});
	advance(500);
	EXPECT_DOUBLE_EQ(world::electrics_of(*hardware(), calibration).current_a, idle);
}

TEST_F(ServoTravelTest, EachCommandCountsAsAMovementSoItsTimingIsDrawnAfresh) {
	EXPECT_EQ(hardware()->servo_movement_count(), 0U);

	world::command_servo(*hardware(), module::ServoAngle{90.0});
	EXPECT_EQ(hardware()->servo_movement_count(), 1U);

	world::command_servo(*hardware(), module::ServoAngle{0.0});
	EXPECT_EQ(hardware()->servo_movement_count(), 2U);

	// Placing an arm is not a movement, so there is nothing to time.
	hardware()->set_servo_angle(module::ServoAngle{45.0});
	EXPECT_EQ(hardware()->servo_movement_count(), 2U);
}

TEST_F(ServoTravelTest, TwoModulesUnderOneSeedDoNotSweepInStep) {
	// Per-module offsets mean identically commanded arms do not arrive together.
	world::set_calibration_seed(20260806);
	world_.add_module(module::ModuleId{2});
	settings::ModuleSettings active{};
	active.active = true;
	settings::set_module_settings(module::ModuleId{2}, active);

	module::Module* first  = world_.module(module::ModuleId{MODULE});
	module::Module* second = world_.module(module::ModuleId{2});

	world::command_servo(*first, module::ServoAngle{180.0});
	world::command_servo(*second, module::ServoAngle{180.0});

	int first_arrival  = -1;
	int second_arrival = -1;
	for (int tick = 0; tick < 5000; tick++) {
		advance(1);
		if (first_arrival < 0 && !first->is_servo_moving()) {
			first_arrival = tick;
		}
		if (second_arrival < 0 && !second->is_servo_moving()) {
			second_arrival = tick;
		}
	}

	ASSERT_GT(first_arrival, 0);
	ASSERT_GT(second_arrival, 0);
	EXPECT_NE(first_arrival, second_arrival);
}
