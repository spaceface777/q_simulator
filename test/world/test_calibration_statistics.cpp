#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <vector>

#include "../../src/module/module.h"
#include "../../src/settings.h"
#include "../../src/world/world.h"

// Checks that the simulated calibration population has the shape the physical bench measured.

namespace {

	using module::ModuleId;
	using world::Calibration;
	using world::Population;

	constexpr double UM_PER_M = 1000000.0;

	// Loose on purpose: clipping to the observed bounds pulls the sample mean and spread in.
	constexpr double MEAN_TOLERANCE_SIGMAS = 0.5;
	constexpr double SD_MIN_FRACTION       = 0.4;
	constexpr double SD_MAX_FRACTION       = 1.2;

	double mean_of(const std::vector<double>& values) {
		double total = 0.0;
		for (const double value : values) {
			total += value;
		}
		return total / static_cast<double>(values.size());
	}

	double sample_sd_of(const std::vector<double>& values) {
		const double average = mean_of(values);
		double       sum_sq  = 0.0;
		for (const double value : values) {
			sum_sq += (value - average) * (value - average);
		}
		return std::sqrt(sum_sq / static_cast<double>(values.size() - 1));
	}

	class CalibrationStatisticsTest : public ::testing::Test {
	protected:
		static constexpr int      SEEDS      = 60;
		static constexpr uint64_t FIRST_SEED = 1000;
		world::CalibrationConfig  config_;

		void SetUp() override {
			world::clear_calibration_seed();
			world::set_calibration_config(config_);
		}

		void TearDown() override {
			world::clear_calibration_seed();
			world::set_calibration_config(world::CalibrationConfig{});
		}

		// Every routing module across many seeds: the population the distribution checks run over.
		static std::vector<Calibration> population() {
			std::vector<Calibration> drawn;
			for (uint64_t seed = FIRST_SEED; seed < FIRST_SEED + SEEDS; seed++) {
				for (uint8_t id = 1; id < MAX_MODULES; id++) {
					drawn.push_back(world::calibration_for(ModuleId{id}, seed));
				}
			}
			return drawn;
		}

		static void expect_matches(const std::vector<double>& samples, const Population& expected, const char* label) {
			ASSERT_GT(samples.size(), 1U) << label;

			for (const double value : samples) {
				EXPECT_GE(value, expected.min) << label << " drew below the lowest module mean the bench saw";
				EXPECT_LE(value, expected.max) << label << " drew above the highest module mean the bench saw";
			}

			EXPECT_NEAR(mean_of(samples), expected.mean, MEAN_TOLERANCE_SIGMAS * expected.sd)
				<< label << " is not centred where the measured population was";

			// Catches a model giving every module the same value, which passes every bound above.
			const double sd = sample_sd_of(samples);
			EXPECT_GT(sd, SD_MIN_FRACTION * expected.sd) << label << " has less spread than the measured population";
			EXPECT_LT(sd, SD_MAX_FRACTION * expected.sd) << label << " has more spread than the measured population";
		}

		static double top_speed_m_per_s(const world::BeltCurve& curve, bool leftward) {
			return std::fabs(world::belt_speed_um_per_s(curve, leftward ? 100.0 : -100.0)) / UM_PER_M;
		}
	};

} // namespace

// =================================================================================================
// Determinism and per-module identity
// =================================================================================================

TEST_F(CalibrationStatisticsTest, ACalibrationIsFixedByItsSeedAndModuleAlone) {
	// calibration_for is a pure function of module and seed, drawn once and never resampled.
	const Calibration first  = world::calibration_for(ModuleId{6}, 424242);
	const Calibration second = world::calibration_for(ModuleId{6}, 424242);

	EXPECT_DOUBLE_EQ(first.electrical.idle_current_a, second.electrical.idle_current_a);
	EXPECT_DOUBLE_EQ(first.electrical.idle_voltage_v, second.electrical.idle_voltage_v);
	EXPECT_DOUBLE_EQ(first.electrical.max_load_voltage_sag_v, second.electrical.max_load_voltage_sag_v);
	EXPECT_DOUBLE_EQ(first.servo.module_offset_ms, second.servo.module_offset_ms);
	EXPECT_EQ(first.mechanical.big_belt.weak_motor, second.mechanical.big_belt.weak_motor);
	EXPECT_EQ(first.mechanical.big_belt.left_um_per_s, second.mechanical.big_belt.left_um_per_s);
}

TEST_F(CalibrationStatisticsTest, ADifferentSeedIsADifferentFleet) {
	// If it were not, the seed would be decorative and every run would meet the same modules.
	const Calibration first  = world::calibration_for(ModuleId{6}, 424242);
	const Calibration second = world::calibration_for(ModuleId{6}, 424243);

	EXPECT_NE(first.electrical.idle_current_a, second.electrical.idle_current_a);
	EXPECT_NE(first.electrical.idle_voltage_v, second.electrical.idle_voltage_v);
	EXPECT_TRUE(first.hardware.right_laser_position_um != second.hardware.right_laser_position_um ||
				first.hardware.left_laser_position_um != second.hardware.left_laser_position_um ||
				first.hardware.rfid_position_um != second.hardware.rfid_position_um);
}

TEST_F(CalibrationStatisticsTest, HardwarePositionsArePersistentIndependentNormalDraws) {
	const Calibration first  = world::calibration_for(ModuleId{6}, 424242);
	const Calibration second = world::calibration_for(ModuleId{6}, 424242);
	EXPECT_EQ(first.hardware.right_laser_position_um, second.hardware.right_laser_position_um);
	EXPECT_EQ(first.hardware.left_laser_position_um, second.hardware.left_laser_position_um);
	EXPECT_EQ(first.hardware.rfid_position_um, second.hardware.rfid_position_um);

	std::vector<double> right;
	std::vector<double> left;
	std::vector<double> rfid;
	for (const Calibration& calibration : population()) {
		right.push_back(static_cast<double>(calibration.hardware.right_laser_position_um));
		left.push_back(static_cast<double>(calibration.hardware.left_laser_position_um));
		rfid.push_back(static_cast<double>(calibration.hardware.rfid_position_um));
	}

	const double leg               = static_cast<double>(Calibration::nominal().endpoint_to_junction_um);
	const double big_belt_length   = 2.0 * leg;
	const double small_belt_length = leg;
	EXPECT_NEAR(mean_of(right), config_.right_laser_position_mean * big_belt_length, 0.01 * leg);
	EXPECT_NEAR(mean_of(left), config_.left_laser_position_mean * big_belt_length, 0.01 * leg);
	EXPECT_NEAR(mean_of(rfid), config_.rfid_position_mean * small_belt_length, 0.02 * leg);
	EXPECT_NEAR(sample_sd_of(right), config_.right_laser_position_sd * big_belt_length, 0.01 * leg);
	EXPECT_NEAR(sample_sd_of(left), config_.left_laser_position_sd * big_belt_length, 0.01 * leg);
	EXPECT_NEAR(sample_sd_of(rfid), config_.rfid_position_sd * small_belt_length, 0.02 * leg);
	EXPECT_NE(right, left);
	EXPECT_NE(right, rfid);
	EXPECT_NE(left, rfid);
}

TEST_F(CalibrationStatisticsTest, ModulesUnderOneSeedAreNotCloneOfEachOther) {
	// Replaces old behaviour where calibration_for ignored the module id and returned one constant.
	constexpr uint64_t SEED = 7;
	std::set<double>   idle_currents;
	std::set<double>   idle_voltages;
	std::set<double>   servo_offsets;
	for (uint8_t id = 1; id < MAX_MODULES; id++) {
		const Calibration calibration = world::calibration_for(ModuleId{id}, SEED);
		idle_currents.insert(calibration.electrical.idle_current_a);
		idle_voltages.insert(calibration.electrical.idle_voltage_v);
		servo_offsets.insert(calibration.servo.module_offset_ms);
	}

	EXPECT_EQ(idle_currents.size(), MAX_MODULES - 1U);
	EXPECT_EQ(idle_voltages.size(), MAX_MODULES - 1U);
	EXPECT_EQ(servo_offsets.size(), MAX_MODULES - 1U);
}

TEST_F(CalibrationStatisticsTest, ThereIsNoWayToAskForAPlatformOfIdenticalModules) {
	// Per-module variation is not opt-in: even without a chosen seed, a caller still gets a fleet.
	const Calibration first  = world::calibration_for(ModuleId{1});
	const Calibration second = world::calibration_for(ModuleId{17});

	EXPECT_NE(first.electrical.idle_current_a, second.electrical.idle_current_a);
	EXPECT_NE(first.servo.module_offset_ms, second.servo.module_offset_ms);

	// Choosing a seed chooses a different fleet, not the presence of one.
	world::set_calibration_seed(99);
	EXPECT_NE(world::calibration_for(ModuleId{1}).electrical.idle_current_a, first.electrical.idle_current_a);
}

TEST_F(CalibrationStatisticsTest, NoModuleIdIsSpecialAndModulesAreExchangeableAcrossSeeds) {
	// Any two ids must draw from the same distribution, or a timeout could be hardcoded per id.
	std::vector<double> idle_by_module;
	std::vector<double> top_speed_by_module;
	std::vector<double> weak_rate_by_module;

	for (uint8_t id = 1; id < MAX_MODULES; id++) {
		std::vector<double> idle;
		std::vector<double> top;
		int                 weak = 0;
		for (uint64_t seed = FIRST_SEED; seed < FIRST_SEED + SEEDS; seed++) {
			const Calibration calibration = world::calibration_for(ModuleId{id}, seed);
			idle.push_back(calibration.electrical.idle_current_a);
			top.push_back(top_speed_m_per_s(calibration.mechanical.big_belt, true));
			weak += calibration.mechanical.big_belt.weak_motor ? 1 : 0;
		}
		idle_by_module.push_back(mean_of(idle));
		top_speed_by_module.push_back(mean_of(top));
		weak_rate_by_module.push_back(static_cast<double>(weak) / static_cast<double>(SEEDS));
	}

	// The spread between module averages should be sampling noise, not a difference between ids.
	EXPECT_LT(sample_sd_of(idle_by_module), 0.25 * world::IDLE_CURRENT_A.sd);
	EXPECT_LT(sample_sd_of(top_speed_by_module), 0.25 * 0.00803);
	EXPECT_LT(sample_sd_of(weak_rate_by_module), 0.12);

	// No id is consistently the fastest: the winner changes with the seed.
	std::set<uint8_t> fastest_ids;
	for (uint64_t seed = FIRST_SEED; seed < FIRST_SEED + SEEDS; seed++) {
		uint8_t fastest = 1;
		double  best    = 0.0;
		for (uint8_t id = 1; id < MAX_MODULES; id++) {
			const double top = top_speed_m_per_s(world::calibration_for(ModuleId{id}, seed).mechanical.big_belt, true);
			if (top > best) {
				best    = top;
				fastest = id;
			}
		}
		fastest_ids.insert(fastest);
	}
	EXPECT_GT(fastest_ids.size(), 5U) << "the same module id keeps winning, so ids are not exchangeable";
}

TEST_F(CalibrationStatisticsTest, AWholeRunMayHaveNoWeakModulesOrNothingButWeakModules) {
	// Drawn per module per seed rather than a fixed quota, so a run can have none weak or all weak.
	std::set<int> counts;
	for (uint64_t seed = 0; seed < 10000; seed++) {
		int weak = 0;
		for (uint8_t id = 1; id < MAX_MODULES; id++) {
			weak += world::calibration_for(ModuleId{id}, seed).mechanical.big_belt.weak_motor ? 1 : 0;
		}
		counts.insert(weak);
	}

	// Over four hundred fleets the number of weak modules varies widely, including some with none.
	EXPECT_GT(counts.size(), 6U);
	EXPECT_EQ(counts.count(0), 1U) << "no run ever came up with a fleet of entirely healthy modules";
}

// =================================================================================================
// Electrical distributions
// =================================================================================================

TEST_F(CalibrationStatisticsTest, EveryMeasuredElectricalQuantityMatchesItsPopulation) {
	const std::vector<Calibration> drawn = population();

	const auto collect = [&drawn](double world::Electrical::* field) {
		std::vector<double> values;
		values.reserve(drawn.size());
		for (const Calibration& calibration : drawn) {
			values.push_back(calibration.electrical.*field);
		}
		return values;
	};

	expect_matches(collect(&world::Electrical::idle_current_a), world::IDLE_CURRENT_A, "idle current");
	expect_matches(collect(&world::Electrical::idle_voltage_v), world::IDLE_VOLTAGE_V, "idle voltage");
	expect_matches(collect(&world::Electrical::left_laser_current_a), world::LEFT_LASER_CURRENT_A, "left laser");
	expect_matches(collect(&world::Electrical::right_laser_current_a), world::RIGHT_LASER_CURRENT_A, "right laser");
	expect_matches(collect(&world::Electrical::led_current_a_at_full_white), world::WHITE_LED_CURRENT_A, "white LED");
	expect_matches(collect(&world::Electrical::big_belt_current_a), world::BIG_BELT_CURRENT_A, "big belt current");
	expect_matches(collect(&world::Electrical::small_belt_current_a), world::SMALL_BELT_CURRENT_A,
				   "small belt current");
	expect_matches(collect(&world::Electrical::max_load_voltage_sag_v), world::MAX_LOAD_VOLTAGE_SAG_V, "voltage sag");
}

TEST_F(CalibrationStatisticsTest, TheTwoLasersAreDrawnSeparatelyRatherThanOnceAndReused) {
	// The bench measured the two sides as different distributions: different means, different
	// spreads, and a lower minimum on the right. A model that drew one laser value and used it
	// twice would satisfy each side's distribution check on its own while never producing a
	// module whose two emitters differ, which is every real module.
	int asymmetric = 0;
	int total      = 0;
	for (const Calibration& calibration : population()) {
		total++;
		if (calibration.electrical.left_laser_current_a != calibration.electrical.right_laser_current_a) {
			asymmetric++;
		}
	}
	EXPECT_EQ(asymmetric, total);
}

TEST_F(CalibrationStatisticsTest, TheReportedIdleCurrentCanFallOutsideWhatADiagnosticAccepts) {
	// Worth pinning, because it is a real consequence of using the measured population and not a
	// defect. hardware_test accepts idle current between 0.06 and 0.10 A, and the bench's own
	// filtered nominal population reaches down to 0.0583 A - module 4 drew less than the applet
	// that inspects it is willing to accept. A fleet drawn from the measured population will
	// therefore contain modules that fail an applet's idle check, exactly as the shelf does.
	//
	// This is why scenarios opt in to per-module calibration rather than inheriting it.
	constexpr double HARDWARE_TEST_IDLE_CURRENT_MIN_A = 0.06;
	EXPECT_LT(world::IDLE_CURRENT_A.min, HARDWARE_TEST_IDLE_CURRENT_MIN_A);

	int below = 0;
	for (const Calibration& calibration : population()) {
		if (calibration.electrical.idle_current_a < HARDWARE_TEST_IDLE_CURRENT_MIN_A) {
			below++;
		}
	}
	EXPECT_GT(below, 0) << "the sampled population never reaches the low tail the bench observed";
}

// =================================================================================================
// Voltage sag
// =================================================================================================

TEST_F(CalibrationStatisticsTest, TheSupplyInterpolatesBetweenIdleAndTheModulesOwnMaximumSag) {
	// Only the two endpoints were measured. The straight line between them is an assumption, so
	// what is checked is the endpoints, the direction in between, and that it is this module's
	// own sampled sag rather than a fleet constant.
	world::State             world;
	settings::ModuleSettings active{};
	active.active = true;
	settings::set_module_settings(ModuleId{1}, active);
	world.add_module(ModuleId{1});

	const Calibration calibration = world::calibration_for(ModuleId{1}, 99);
	module::Module*   hardware    = world.module(ModuleId{1});

	const double idle_v = world::electrics_of(*hardware, calibration).voltage_v;
	EXPECT_NEAR(idle_v, calibration.electrical.idle_voltage_v, 0.01);

	hardware->set_big_belt_speed(module::BeltSpeed{100.0});
	hardware->set_small_belt_speed(module::BeltSpeed{100.0});
	hardware->set_servo_angle(module::ServoAngle{180.0});
	hardware->set_led_color(module::LedColor{0xFFFFFFu});
	hardware->set_left_laser_enabled(true);
	hardware->set_right_laser_enabled(true);
	const double max_load_v = world::electrics_of(*hardware, calibration).voltage_v;

	EXPECT_LT(max_load_v, idle_v);
	EXPECT_NEAR(idle_v - max_load_v, calibration.electrical.max_load_voltage_sag_v, 0.01);

	// A partial load sags partially. A step function dressed up as an interpolation would put
	// this at one endpoint or the other.
	hardware->set_small_belt_speed(module::BeltSpeed{0.0});
	hardware->set_led_color(module::LedColor{0x000000u});
	hardware->set_left_laser_enabled(false);
	hardware->set_right_laser_enabled(false);
	const double partial_v = world::electrics_of(*hardware, calibration).voltage_v;
	EXPECT_LT(partial_v, idle_v);
	EXPECT_GT(partial_v, max_load_v);

	world.clear();
	settings::set_module_settings(ModuleId{1}, settings::ModuleSettings{});
}

// =================================================================================================
// Belt speed curves
// =================================================================================================

TEST_F(CalibrationStatisticsTest, ABeltCurveIsSignedByItsCommandAndInterpolatesBetweenItsPoints) {
	const world::BeltCurve& curve = Calibration::nominal().mechanical.big_belt;

	EXPECT_DOUBLE_EQ(world::belt_speed_um_per_s(curve, 0.0), 0.0);
	EXPECT_GT(world::belt_speed_um_per_s(curve, 100.0), 0.0);
	EXPECT_LT(world::belt_speed_um_per_s(curve, -100.0), 0.0);

	// Halfway between two curve points is halfway between their two speeds.
	const double at_40 = std::fabs(world::belt_speed_um_per_s(curve, 40.0));
	const double at_60 = std::fabs(world::belt_speed_um_per_s(curve, 60.0));
	EXPECT_NEAR(std::fabs(world::belt_speed_um_per_s(curve, 50.0)), (at_40 + at_60) / 2.0, 1e-6);

	// Below the lowest measured command the curve runs down to a standstill at zero, which is the
	// least trustworthy part of it: nothing was measured between 0 and 30 percent, and that is
	// exactly where the bend is sharpest.
	const double at_30 = std::fabs(world::belt_speed_um_per_s(curve, 30.0));
	EXPECT_NEAR(std::fabs(world::belt_speed_um_per_s(curve, 15.0)), at_30 / 2.0, 1e-6);
}

TEST_F(CalibrationStatisticsTest, SampledCurvesAreStronglyNonlinearAtLowCommands) {
	// The report's headline finding: a 30 percent command does not produce 30 percent of top
	// speed, and how far off it is varies enormously between modules. The bench saw 49, 57, 20,
	// 66 and 30 percent across its five. A model that scaled a maximum speed linearly by command
	// would put every one of these at 0.30 with no spread at all.
	std::vector<double> ratios;
	for (const Calibration& calibration : population()) {
		const world::BeltCurve& curve = calibration.mechanical.big_belt;
		ratios.push_back(std::fabs(world::belt_speed_um_per_s(curve, 30.0)) /
						 std::fabs(world::belt_speed_um_per_s(curve, 100.0)));
	}

	// The report's own figures for that population, restated rather than read out of the model:
	// mean 0.444 with a between-module sd of 0.190.
	EXPECT_NEAR(mean_of(ratios), 0.444, 0.12);
	EXPECT_GT(sample_sd_of(ratios), 0.5 * 0.190);

	// The extremes of the measured population are reachable: some modules barely move off the
	// line while others are already two thirds of the way to full speed.
	const double lowest  = *std::min_element(ratios.begin(), ratios.end());
	const double highest = *std::max_element(ratios.begin(), ratios.end());
	EXPECT_LT(lowest, 0.25);
	EXPECT_GT(highest, 0.60);
	EXPECT_GT(highest, 3.0 * lowest);
}

TEST_F(CalibrationStatisticsTest, SampledCurvesAreMonotonicAndStayInsideTheMeasuredRange) {
	// An unmeasured module must still be a motor. Ten independent per-point draws would
	// routinely produce a belt that runs slower at 80 percent than at 60, and no belt does that.
	for (const Calibration& calibration : population()) {
		for (const bool leftward : {true, false}) {
			double previous = 0.0;
			for (const double command : {30.0, 40.0, 60.0, 80.0, 100.0}) {
				const double speed = std::fabs(world::belt_speed_um_per_s(calibration.mechanical.big_belt,
																		  leftward ? command : -command)) /
									 UM_PER_M;
				EXPECT_GE(speed, previous) << "a belt curve fell as its command rose";
				previous = speed;
			}
			// No simulated belt runs faster than the fastest the bench timed.
			EXPECT_LE(previous, 0.0976 + 1e-9);
		}
	}
}

TEST_F(CalibrationStatisticsTest, SampledModulesAreDistinguishableRatherThanNominalClones) {
	// The report is explicit that unmeasured modules must not be made nominal clones of each
	// other, and that a separate left/right multiplier should survive where the sample supports
	// it.
	std::set<double> top_speeds;
	int              faster_rightward = 0;
	int              total            = 0;
	for (const Calibration& calibration : population()) {
		total++;
		const double left  = top_speed_m_per_s(calibration.mechanical.big_belt, true);
		const double right = top_speed_m_per_s(calibration.mechanical.big_belt, false);
		top_speeds.insert(left);
		EXPECT_NE(left, right) << "a sampled module lost its direction asymmetry";
		if (right > left) {
			faster_rightward++;
		}
	}

	EXPECT_GT(top_speeds.size(), static_cast<std::size_t>(total) / 2);

	// Every module the bench timed was at least slightly faster rightward, so the sampled
	// population should lean the same way rather than splitting evenly.
	EXPECT_GT(faster_rightward, total / 2);
}

// =================================================================================================
// Weak motors
// =================================================================================================

TEST_F(CalibrationStatisticsTest, WeakMotorsAppearAtTheConfiguredRate) {
	// A population prior describing how many modules on a shelf are stiff, not a failure rate
	// measured from the five benchmarked ones.
	const std::vector<Calibration> drawn = population();
	int                            weak  = 0;
	for (const Calibration& calibration : drawn) {
		if (calibration.mechanical.big_belt.weak_motor) {
			weak++;
		}
	}

	const double rate = static_cast<double>(weak) / static_cast<double>(drawn.size());
	EXPECT_NEAR(rate, world::calibration_config().weak_motor_rate, 0.05);

	// Degenerate in either direction would mean the prior is not being applied at all.
	EXPECT_GT(weak, 0);
	EXPECT_LT(weak, static_cast<int>(drawn.size()));
}

TEST_F(CalibrationStatisticsTest, TheWeakMotorRateIsConfigurable) {
	const auto measured_rate = []() {
		const std::vector<Calibration> drawn = population();
		int                            weak  = 0;
		for (const Calibration& calibration : drawn) {
			if (calibration.mechanical.big_belt.weak_motor) {
				weak++;
			}
		}
		return static_cast<double>(weak) / static_cast<double>(drawn.size());
	};

	world::CalibrationConfig none;
	none.weak_motor_rate = 0.0;
	world::set_calibration_config(none);
	EXPECT_DOUBLE_EQ(measured_rate(), 0.0);

	world::CalibrationConfig all;
	all.weak_motor_rate = 1.0;
	world::set_calibration_config(all);
	EXPECT_DOUBLE_EQ(measured_rate(), 1.0);

	world::CalibrationConfig half;
	half.weak_motor_rate = 0.5;
	world::set_calibration_config(half);
	EXPECT_NEAR(measured_rate(), 0.5, 0.05);
}

TEST_F(CalibrationStatisticsTest, AWeakMotorIsOneThatCannotGetStartedRatherThanMerelyASlowOne) {
	// The report's emphasis: a modestly lower top speed, but strongly attenuated and variable
	// response below roughly half command, where such a motor may crawl or stall. A model that
	// only scaled the whole curve down would leave the low-command ratio unchanged, so the ratio
	// is what is checked rather than the speeds.
	std::vector<double> weak_ratios;
	std::vector<double> healthy_ratios;
	std::vector<double> weak_tops;
	std::vector<double> healthy_tops;

	for (const Calibration& calibration : population()) {
		const world::BeltCurve& curve = calibration.mechanical.big_belt;
		const double            top   = top_speed_m_per_s(curve, true);
		const double            ratio = std::fabs(world::belt_speed_um_per_s(curve, 30.0)) / UM_PER_M / top;
		if (calibration.mechanical.big_belt.weak_motor) {
			weak_ratios.push_back(ratio);
			weak_tops.push_back(top);
		} else {
			healthy_ratios.push_back(ratio);
			healthy_tops.push_back(top);
		}
	}

	ASSERT_FALSE(weak_ratios.empty());
	ASSERT_FALSE(healthy_ratios.empty());

	// The low-command penalty is the large effect.
	EXPECT_LT(mean_of(weak_ratios), 0.5 * mean_of(healthy_ratios));

	// The top-speed penalty is the small one, and must not be the whole story: a weak motor
	// still gets there at full command.
	EXPECT_LT(mean_of(weak_tops), mean_of(healthy_tops));
	EXPECT_GT(mean_of(weak_tops), 0.7 * mean_of(healthy_tops));
}

// =================================================================================================
// The small belt
// =================================================================================================

TEST_F(CalibrationStatisticsTest, TheSmallBeltBorrowsTheBigBeltsShapeAndTheProvisionalRatio) {
	// No small-belt travel time has ever been measured. Until one is, the small belt must track
	// the big belt's per-module top speed and directionality through one documented ratio, rather
	// than being sampled as an unrelated motor - and it must not be inferred from the small-belt
	// current, which is the one small-belt number that does exist.
	//
	// Weak motors would break this direct comparison (they are sampled independently below), so
	// this checks the shared shape in isolation by forcing every motor healthy.
	world::CalibrationConfig config;
	config.weak_motor_rate = 0.0;
	world::set_calibration_config(config);

	const double ratio = world::calibration_config().small_to_big_belt_speed_ratio;

	for (uint8_t id = 1; id < MAX_MODULES; id++) {
		const Calibration calibration = world::calibration_for(ModuleId{id}, 8675309);
		for (const double command : {30.0, 60.0, 100.0, -30.0, -60.0, -100.0}) {
			const double big   = world::belt_speed_um_per_s(calibration.mechanical.big_belt, command);
			const double small = world::belt_speed_um_per_s(calibration.mechanical.small_belt, command);
			EXPECT_NEAR(small, big * ratio, 1e-6) << "module " << int{id} << " at " << command << " percent";
		}
	}
}

TEST_F(CalibrationStatisticsTest, WeakMotorIsSampledIndependentlyForEachBelt) {
	// The report describes one motor per belt, big and small, each of which can independently be
	// weak or healthy: a module can have neither weak, only one, or both. The field belongs on
	// BeltCurve, not on Mechanical, so it is possible to represent - and the sampler must actually
	// produce - all four combinations rather than copying the big belt's status onto the small
	// belt's.
	int both_weak = 0, only_big = 0, only_small = 0, neither = 0;
	for (uint64_t seed = FIRST_SEED; seed < FIRST_SEED + SEEDS; seed++) {
		for (uint8_t id = 1; id < MAX_MODULES; id++) {
			const Calibration calibration = world::calibration_for(ModuleId{id}, seed);
			const bool        big_weak    = calibration.mechanical.big_belt.weak_motor;
			const bool        small_weak  = calibration.mechanical.small_belt.weak_motor;
			if (big_weak && small_weak) {
				both_weak++;
			} else if (big_weak) {
				only_big++;
			} else if (small_weak) {
				only_small++;
			} else {
				neither++;
			}
		}
	}

	EXPECT_GT(both_weak, 0) << "both motors were never weak together";
	EXPECT_GT(only_big, 0) << "the big motor was never weak on its own";
	EXPECT_GT(only_small, 0) << "the small motor was never weak on its own";
	EXPECT_GT(neither, 0) << "both motors were never healthy together";
}

TEST_F(CalibrationStatisticsTest, ASmallBeltMotorsWeaknessActuallyAttenuatesTheSmallBeltsOwnCurve) {
	// Independent sampling would be pointless if the small belt's curve did not actually reflect
	// its own weak status. This pins that a weak small motor, paired with a healthy big motor,
	// still shows the low-command attenuation and top-speed penalty on the small belt alone.
	// Can't force one belt weak and the other healthy through the public rate alone, so scan the
	// default-rate population for a module that happened to land that way.
	world::set_calibration_config(world::CalibrationConfig{});

	std::optional<Calibration> found;
	for (uint64_t seed = FIRST_SEED; seed < FIRST_SEED + SEEDS && !found; seed++) {
		for (uint8_t id = 1; id < MAX_MODULES; id++) {
			const Calibration calibration = world::calibration_for(ModuleId{id}, seed);
			if (calibration.mechanical.small_belt.weak_motor && !calibration.mechanical.big_belt.weak_motor) {
				found = calibration;
				break;
			}
		}
	}

	ASSERT_TRUE(found.has_value()) << "no module in range drew a weak small motor with a healthy big motor";

	const world::BeltCurve& small = found->mechanical.small_belt;
	const world::BeltCurve& big   = found->mechanical.big_belt;

	const double small_top = top_speed_m_per_s(small, true);
	const double small_low = std::fabs(world::belt_speed_um_per_s(small, 30.0)) / UM_PER_M;
	const double big_top   = top_speed_m_per_s(big, true);
	const double big_low   = std::fabs(world::belt_speed_um_per_s(big, 30.0)) / UM_PER_M;

	EXPECT_LT(small_low / small_top, 0.5 * (big_low / big_top))
		<< "a weak small motor did not show the low-command penalty on its own curve";
}

TEST_F(CalibrationStatisticsTest, TheSmallToBigBeltRatioIsConfigurable) {
	world::CalibrationConfig config;
	config.small_to_big_belt_speed_ratio = 0.5;
	// Weak status is now sampled independently per belt, which would break the direct comparison
	// below if the two motors happened to differ; force both healthy to isolate the ratio.
	config.weak_motor_rate = 0.0;
	world::set_calibration_config(config);

	const Calibration calibration = world::calibration_for(ModuleId{3}, 11);
	EXPECT_NEAR(world::belt_speed_um_per_s(calibration.mechanical.small_belt, 100.0),
				world::belt_speed_um_per_s(calibration.mechanical.big_belt, 100.0) * 0.5, 1e-6);
}

// =================================================================================================
// The belt command floor
// =================================================================================================

TEST_F(CalibrationStatisticsTest, ABeltIsEitherStoppedOrCommandedAboveAQuarter) {
	// A belt is either stopped at zero or commanded between a quarter and full. A nonzero
	// request below that must not silently produce a plausible slow belt, because the physical
	// platform does not produce one.
	EXPECT_TRUE(world::is_valid_belt_command_percent(0.0));
	EXPECT_TRUE(world::is_valid_belt_command_percent(-0.0));

	for (const double magnitude : {25.0, 25.5, 40.0, 80.0, 100.0}) {
		EXPECT_TRUE(world::is_valid_belt_command_percent(magnitude)) << magnitude;
		EXPECT_TRUE(world::is_valid_belt_command_percent(-magnitude)) << magnitude;
	}

	for (const double magnitude : {0.001, 1.0, 5.0, 10.0, 24.999}) {
		EXPECT_FALSE(world::is_valid_belt_command_percent(magnitude)) << magnitude;
		EXPECT_FALSE(world::is_valid_belt_command_percent(-magnitude)) << magnitude;
	}

	for (const double magnitude : {100.001, 150.0}) {
		EXPECT_FALSE(world::is_valid_belt_command_percent(magnitude)) << magnitude;
		EXPECT_FALSE(world::is_valid_belt_command_percent(-magnitude)) << magnitude;
	}

	EXPECT_FALSE(world::is_valid_belt_command_percent(std::nan("")));
	EXPECT_FALSE(world::is_valid_belt_command_percent(INFINITY));
}

// =================================================================================================
// Servo timing
// =================================================================================================

TEST_F(CalibrationStatisticsTest, ANominalServoTakesTheProvisionalSweepTimeExactly) {
	// Provisional, and labelled as such everywhere it appears: no servo measurement survives
	// either bench run, and the report's conclusion is that CV current cannot observe servo
	// movement on this hardware at all.
	const Calibration nominal = Calibration::nominal();

	EXPECT_EQ(world::servo_travel_ticks(nominal, 0.0, 180.0, 0, ModuleId{1}, 0), 500);
	EXPECT_EQ(world::servo_travel_ticks(nominal, 0.0, 90.0, 0, ModuleId{1}, 0), 250);
	EXPECT_EQ(world::servo_travel_ticks(nominal, 0.0, 60.0, 0, ModuleId{1}, 0), 167);
	EXPECT_EQ(world::servo_travel_ticks(nominal, 180.0, 0.0, 0, ModuleId{1}, 0), 500);
	EXPECT_EQ(world::servo_travel_ticks(nominal, 90.0, 90.0, 0, ModuleId{1}, 0), 0);

	// A degree is 2.78 ms at this sweep time, to the tick.
	EXPECT_EQ(world::servo_travel_ticks(nominal, 0.0, 1.0, 0, ModuleId{1}, 0), 3);
}

TEST_F(CalibrationStatisticsTest, TheServoSweepTimeIsConfigurable) {
	world::CalibrationConfig config;
	config.servo_full_sweep_ms = 400.0;
	world::set_calibration_config(config);

	const Calibration nominal = Calibration::nominal();
	EXPECT_EQ(world::servo_travel_ticks(nominal, 0.0, 180.0, 0, ModuleId{1}, 0), 400);
	EXPECT_EQ(world::servo_travel_ticks(nominal, 0.0, 90.0, 0, ModuleId{1}, 0), 200);
}

TEST_F(CalibrationStatisticsTest, EachServoHasAPersistentOffsetOfItsOwn) {
	// Fixed per module and sampled once: this particular servo is persistently a little faster
	// or slower than the fleet, the way a real one is. It is not resampled per movement, so the
	// same module asked the same question twice with no jitter answers the same way.
	world::CalibrationConfig config;
	config.servo_movement_jitter_sd_ms = 0.0;
	world::set_calibration_config(config);

	std::vector<double> offsets;
	std::set<int64_t>   sweep_times;
	for (uint8_t id = 1; id < MAX_MODULES; id++) {
		const Calibration calibration = world::calibration_for(ModuleId{id}, 4242);
		offsets.push_back(calibration.servo.module_offset_ms);
		const int64_t first  = world::servo_travel_ticks(calibration, 0.0, 180.0, 4242, ModuleId{id}, 0);
		const int64_t second = world::servo_travel_ticks(calibration, 0.0, 180.0, 4242, ModuleId{id}, 1);
		EXPECT_EQ(first, second) << "module " << int{id} << " changed pace with no jitter configured";
		sweep_times.insert(first);
	}

	// Centred on zero, so a module is as likely to be quicker than the fleet as slower and the
	// fleet mean stays the configured sweep time rather than drifting off it.
	EXPECT_NEAR(mean_of(offsets), 0.0, 60.0);
	EXPECT_NEAR(sample_sd_of(offsets), world::CalibrationConfig{}.servo_module_offset_sd_ms, 45.0);

	// Both signs occur: some modules really are faster than nominal, not merely less slow.
	EXPECT_LT(*std::min_element(offsets.begin(), offsets.end()), 0.0);
	EXPECT_GT(*std::max_element(offsets.begin(), offsets.end()), 0.0);

	// And the modules really do differ from each other.
	EXPECT_GT(sweep_times.size(), 1U);
}

TEST_F(CalibrationStatisticsTest, TheSameServoDoesNotTakeTheSameTimeTwice) {
	// The per-movement layer, which is a different physical claim from the per-module one: the
	// same arm does not repeat itself exactly. Without this an application could race a servo
	// against a belt and pass by relying on a timing that only holds in simulation.
	world::set_calibration_seed(31337);
	const Calibration calibration = world::calibration_for(ModuleId{3});
	ASSERT_GT(calibration.servo.movement_jitter_sd_ms, 0.0);

	std::vector<double> durations;
	std::set<int64_t>   distinct;
	for (uint64_t movement = 0; movement < 400; movement++) {
		const int64_t ticks = world::servo_travel_ticks(calibration, 0.0, 180.0, 31337, ModuleId{3}, movement);
		durations.push_back(static_cast<double>(ticks));
		distinct.insert(ticks);
	}

	EXPECT_GT(distinct.size(), 50U) << "the per-movement draw is not varying";

	// Centred on this module's own pace, spread by the configured per-sweep jitter.
	const double expected_mean = calibration.servo.full_sweep_ms + calibration.servo.module_offset_ms;
	EXPECT_NEAR(mean_of(durations), expected_mean, 15.0);
	EXPECT_NEAR(sample_sd_of(durations), world::CalibrationConfig{}.servo_movement_jitter_sd_ms, 15.0);
}

TEST_F(CalibrationStatisticsTest, ServoJitterScalesWithTheDistanceTravelled) {
	// Both variance terms are quoted per full sweep and scale with the fraction travelled, so a
	// servo that is ten percent slow is ten percent slow over any distance rather than paying a
	// fixed penalty that would dominate short movements.
	world::set_calibration_seed(31337);
	const Calibration calibration = world::calibration_for(ModuleId{3});

	const auto spread = [&calibration](double degrees) {
		std::vector<double> durations;
		for (uint64_t movement = 0; movement < 400; movement++) {
			durations.push_back(static_cast<double>(
				world::servo_travel_ticks(calibration, 0.0, degrees, 31337, ModuleId{3}, movement)));
		}
		return sample_sd_of(durations);
	};

	const double full    = spread(180.0);
	const double quarter = spread(45.0);

	EXPECT_GT(full, 0.0);
	EXPECT_GT(quarter, 0.0);
	EXPECT_NEAR(quarter, full / 4.0, 0.15 * full);
}

TEST_F(CalibrationStatisticsTest, AServoMovementNeverTakesLessThanNoTime) {
	// However far the jitter falls. A negative duration would arrive before it was commanded.
	world::CalibrationConfig config;
	config.servo_full_sweep_ms         = 10.0;
	config.servo_module_offset_sd_ms   = 0.0;
	config.servo_movement_jitter_sd_ms = 5000.0;
	world::set_calibration_config(config);
	world::set_calibration_seed(5);

	const Calibration calibration = world::calibration_for(ModuleId{2});
	for (uint64_t movement = 0; movement < 2000; movement++) {
		EXPECT_GE(world::servo_travel_ticks(calibration, 0.0, 180.0, 5, ModuleId{2}, movement), 0);
	}
}
