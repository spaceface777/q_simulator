#include <gtest/gtest.h>

#include <cmath>

#include "../../src/module/module.h"
#include "../../src/settings.h"
#include "../../src/world/world.h"

namespace {

	class InstrumentsTest : public ::testing::Test {
	protected:
		static constexpr uint8_t MODULE = 1;

		// hardware_test's acceptance constants, named as it names them.
		static constexpr double  IDLE_VOLTAGE_MIN_V                  = 12.0;
		static constexpr double  IDLE_VOLTAGE_MAX_V                  = 13.0;
		static constexpr double  IDLE_CURRENT_MIN_A                  = 0.06;
		static constexpr double  IDLE_CURRENT_MAX_A                  = 0.10;
		static constexpr double  IDLE_POWER_MIN_W                    = 0.80;
		static constexpr double  IDLE_POWER_MAX_W                    = 1.25;
		static constexpr double  POWER_CONSISTENCY_ABS_TOLERANCE_W   = 0.20;
		static constexpr double  POWER_CONSISTENCY_REL_TOLERANCE     = 0.15;
		static constexpr double  BELT_POWER_100_MIN_RISE_W           = 0.60;
		static constexpr double  BELT_OFF_MAX_IDLE_DELTA_W           = 0.15;
		static constexpr double  SERVO_MIN_POWER_RISE_W              = 0.30;
		static constexpr int64_t BELT_ENCODER_25_MIN_DELTA           = 5;
		static constexpr int64_t BELT_ENCODER_25_MAX_DELTA           = 100;
		static constexpr int64_t BELT_ENCODER_100_MIN_DELTA          = 20;
		static constexpr int64_t BELT_ENCODER_100_MAX_DELTA          = 100;
		static constexpr int64_t BELT_ENCODER_100_MIN_DELTA_ABOVE_25 = 5;
		static constexpr int     BELT_ENCODER_RUN_MS                 = 10000;
		static constexpr double  BELT_LOW_SPEED_PERCENT              = 25.0;
		static constexpr double  BELT_HIGH_SPEED_PERCENT             = 100.0;

		// Restated rather than read from world::electrics_of, so the model is not its own oracle.
		static constexpr double INA226_R_SHUNT_OHM = 0.002;

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
			world_.clear();
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
		}

		module::Module* hardware() { return world_.module(module::ModuleId{MODULE}); }

		world::Electrics reading() {
			return world::electrics_of(*hardware(), world::calibration_for(module::ModuleId{MODULE}));
		}

		void advance(int ticks) {
			for (int index = 0; index < ticks; index++) {
				world_.advance_tick();
			}
		}

		// Runs one belt for as long as hardware_test does, returning the applet's unsigned delta.
		int64_t encoder_delta_over_a_run(bool big, double speed_percent) {
			const world::Calibration calibration = world::calibration_for(module::ModuleId{MODULE});
			const auto               count       = [&]() {
				return world::encoder_count(big ? hardware()->big_belt_travel_um() : hardware()->small_belt_travel_um(),
											calibration);
			};

			const int64_t before = count();
			if (big) {
				hardware()->set_big_belt_speed(module::BeltSpeed{speed_percent});
			} else {
				hardware()->set_small_belt_speed(module::BeltSpeed{speed_percent});
			}
			advance(BELT_ENCODER_RUN_MS);
			if (big) {
				hardware()->set_big_belt_speed(module::BeltSpeed{0.0});
			} else {
				hardware()->set_small_belt_speed(module::BeltSpeed{0.0});
			}
			return std::llabs(count() - before);
		}
	};

} // namespace

TEST_F(InstrumentsTest, AnIdleModuleReadsInsideEveryWindowADiagnosticAccepts) {
	const world::Electrics idle = reading();

	EXPECT_GE(idle.voltage_v, IDLE_VOLTAGE_MIN_V);
	EXPECT_LE(idle.voltage_v, IDLE_VOLTAGE_MAX_V);
	EXPECT_GE(idle.current_a, IDLE_CURRENT_MIN_A);
	EXPECT_LE(idle.current_a, IDLE_CURRENT_MAX_A);
	EXPECT_GE(idle.power_w, IDLE_POWER_MIN_W);
	EXPECT_LE(idle.power_w, IDLE_POWER_MAX_W);
}

TEST_F(InstrumentsTest, PowerAgreesWithVoltageTimesCurrent) {
	// Under load, not only at idle.
	hardware()->set_big_belt_speed(module::BeltSpeed{100.0});
	hardware()->set_servo_angle(module::ServoAngle{180.0});
	hardware()->set_led_color(module::LedColor{0xFFFFFFu});

	const world::Electrics busy     = reading();
	const double           expected = busy.voltage_v * busy.current_a;
	const double tolerance = std::max(POWER_CONSISTENCY_ABS_TOLERANCE_W, POWER_CONSISTENCY_REL_TOLERANCE * expected);
	EXPECT_LE(std::fabs(busy.power_w - expected), tolerance);
}

TEST_F(InstrumentsTest, TheLoadVoltageIncludesTheShuntDropButPowerDoesNot) {
	// The INA226 computes power from bus voltage but reports load voltage.
	hardware()->set_big_belt_speed(module::BeltSpeed{100.0});
	hardware()->set_servo_angle(module::ServoAngle{180.0});
	const world::Electrics busy = reading();

	// Reconstructed per reading, since the supply sags with load.
	const double bus_v = busy.power_w / busy.current_a;
	EXPECT_NEAR(busy.voltage_v, bus_v + busy.current_a * INA226_R_SHUNT_OHM, 1e-9);

	// What a naive voltage-times-current check sees: the shunt's own dissipation, and not zero.
	const double residual = busy.voltage_v * busy.current_a - busy.power_w;
	EXPECT_NEAR(residual, busy.current_a * busy.current_a * INA226_R_SHUNT_OHM, 1e-9);
	EXPECT_GT(residual, 0.0);
}

TEST_F(InstrumentsTest, TheSupplySagsWithLoadAndRecovers) {
	// The bench measured only idle and simultaneous maximum load; in between is interpolated.
	const world::Calibration calibration = world::calibration_for(module::ModuleId{MODULE});
	const double             idle_v      = reading().voltage_v;

	hardware()->set_big_belt_speed(module::BeltSpeed{100.0});
	hardware()->set_small_belt_speed(module::BeltSpeed{100.0});
	hardware()->set_servo_angle(module::ServoAngle{180.0});
	hardware()->set_led_color(module::LedColor{0xFFFFFFu});
	hardware()->set_left_laser_enabled(true);
	hardware()->set_right_laser_enabled(true);
	const double loaded_v = reading().voltage_v;

	// The reported load voltage carries the shunt drop, so the tolerance covers a few hundred uV.
	EXPECT_LT(loaded_v, idle_v);
	EXPECT_NEAR(idle_v - loaded_v, calibration.electrical.max_load_voltage_sag_v, 0.02);

	// Less than maximum load must sag by less than maximum.
	hardware()->set_small_belt_speed(module::BeltSpeed{0.0});
	hardware()->set_led_color(module::LedColor{0x000000u});
	const double partial_v = reading().voltage_v;
	EXPECT_LT(partial_v, idle_v);
	EXPECT_GT(partial_v, loaded_v);

	// And it comes back.
	hardware()->set_big_belt_speed(module::BeltSpeed{0.0});
	hardware()->set_servo_angle(module::ServoAngle{0.0});
	hardware()->set_left_laser_enabled(false);
	hardware()->set_right_laser_enabled(false);
	EXPECT_DOUBLE_EQ(reading().voltage_v, idle_v);
}

TEST_F(InstrumentsTest, ABeltAtFullSpeedRaisesThePowerEnoughToBeSeen) {
	const double idle = reading().power_w;

	for (const bool big : {true, false}) {
		if (big) {
			hardware()->set_big_belt_speed(module::BeltSpeed{BELT_HIGH_SPEED_PERCENT});
		} else {
			hardware()->set_small_belt_speed(module::BeltSpeed{BELT_HIGH_SPEED_PERCENT});
		}

		EXPECT_GE(reading().power_w - idle, BELT_POWER_100_MIN_RISE_W);

		// And stopping it returns to idle.
		hardware()->set_big_belt_speed(module::BeltSpeed{0.0});
		hardware()->set_small_belt_speed(module::BeltSpeed{0.0});
		EXPECT_LE(std::fabs(reading().power_w - idle), BELT_OFF_MAX_IDLE_DELTA_W);
	}
}

TEST_F(InstrumentsTest, ABeltDrawsInProportionToItsCommandedSpeed) {
	// Current, not power: the supply sags with load, so power is not proportional to command.
	// The tolerance is the shunt channel's own 2.5 uV quantum.
	const double current_lsb_a = 0.0000025 / INA226_R_SHUNT_OHM;
	const double idle          = reading().current_a;

	hardware()->set_big_belt_speed(module::BeltSpeed{BELT_HIGH_SPEED_PERCENT});
	const double full = reading().current_a - idle;

	hardware()->set_big_belt_speed(module::BeltSpeed{BELT_LOW_SPEED_PERCENT});
	const double quarter = reading().current_a - idle;

	EXPECT_NEAR(quarter, full / 4.0, 2.0 * current_lsb_a);

	// Draw is direction-independent, even where speed is not.
	hardware()->set_big_belt_speed(module::BeltSpeed{-BELT_HIGH_SPEED_PERCENT});
	EXPECT_NEAR(reading().current_a - idle, full, 1e-9);
}

TEST_F(InstrumentsTest, AServoHeldOffItsRestAngleRaisesThePowerEnoughToBeSeen) {
	const double idle = reading().power_w;

	hardware()->set_servo_angle(module::ServoAngle{180.0});
	EXPECT_GE(reading().power_w - idle, SERVO_MIN_POWER_RISE_W);

	hardware()->set_servo_angle(module::ServoAngle{0.0});
	EXPECT_NEAR(reading().power_w, idle, 1e-9);
}

TEST_F(InstrumentsTest, TheEncodersTurnOutTheCountsADiagnosticExpects) {
	for (const bool big : {true, false}) {
		const int64_t low  = encoder_delta_over_a_run(big, BELT_LOW_SPEED_PERCENT);
		const int64_t high = encoder_delta_over_a_run(big, BELT_HIGH_SPEED_PERCENT);

		EXPECT_GE(low, BELT_ENCODER_25_MIN_DELTA);
		EXPECT_LE(low, BELT_ENCODER_25_MAX_DELTA);
		EXPECT_GE(high, BELT_ENCODER_100_MIN_DELTA);
		EXPECT_LE(high, BELT_ENCODER_100_MAX_DELTA);

		// Faster has to read as faster.
		EXPECT_GE(high, low + BELT_ENCODER_100_MIN_DELTA_ABOVE_25);
	}
}

TEST_F(InstrumentsTest, TheEncoderOnlyEverCountsUp) {
	// The encoder counts the belt turning, not which way, so a reversal adds rather than subtracts.
	const world::Calibration calibration = world::calibration_for(module::ModuleId{MODULE});
	const auto count = [&]() { return world::encoder_count(hardware()->big_belt_travel_um(), calibration); };

	hardware()->set_big_belt_speed(module::BeltSpeed{100.0});
	advance(BELT_ENCODER_RUN_MS);
	const int64_t forwards = count();
	EXPECT_GT(forwards, 0);

	// Not the same distance: the two directions are measured separately and differ.
	hardware()->set_big_belt_speed(module::BeltSpeed{-100.0});
	advance(BELT_ENCODER_RUN_MS);
	const int64_t backwards = count() - forwards;
	EXPECT_GT(backwards, 0);
	EXPECT_EQ(count(), forwards + backwards);

	// And never backwards along the way, not merely at the ends.
	int64_t previous = count();
	for (int index = 0; index < 500; index++) {
		advance(1);
		EXPECT_GE(count(), previous);
		previous = count();
	}
}

TEST_F(InstrumentsTest, TheEncodersTurnWithTheBeltRatherThanWithATub) {
	// A diagnostic runs on a cleared module, so the encoder cannot depend on a tub being present.
	EXPECT_TRUE(world_.tubs().empty());
	EXPECT_GT(encoder_delta_over_a_run(true, BELT_HIGH_SPEED_PERCENT), 0);
}

TEST_F(InstrumentsTest, TheEncoderFrequencyFollowsTheCommandedSpeed) {
	const world::Calibration calibration = world::calibration_for(module::ModuleId{MODULE});
	const auto               freq        = [&](double percent) {
		return world::encoder_frequency_hz(percent, calibration.mechanical.big_belt, calibration);
	};

	EXPECT_DOUBLE_EQ(freq(0.0), 0.0);

	// Faster reads as faster, but not proportionally: the curve is not linear.
	EXPECT_GT(freq(BELT_HIGH_SPEED_PERCENT), freq(BELT_LOW_SPEED_PERCENT));
	EXPECT_GT(freq(BELT_LOW_SPEED_PERCENT), freq(BELT_HIGH_SPEED_PERCENT) / 4.0);

	// A frequency is never negative.
	EXPECT_GT(freq(-BELT_HIGH_SPEED_PERCENT), 0.0);

	// And it agrees with the counts a run of that length produces.
	const double expected_counts = freq(BELT_HIGH_SPEED_PERCENT) * (BELT_ENCODER_RUN_MS / 1000.0);
	EXPECT_EQ(static_cast<int64_t>(expected_counts), encoder_delta_over_a_run(true, BELT_HIGH_SPEED_PERCENT));
}
