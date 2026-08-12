#ifndef WORLD_CALIBRATION_H
#define WORLD_CALIBRATION_H

#include <array>
#include <cstddef>
#include <cstdint>

#include "../module/types.h"

namespace world {
	// Keeps fractional position configuration comfortably inside int64 micrometre arithmetic while
	// still allowing intentionally out-of-belt untruncated-normal experiments.
	inline constexpr double MAX_ABSOLUTE_HARDWARE_POSITION_FRACTION = 1'000'000.0;

	// Real modules differ from nominal. `nominal()` returns one repeatable module; `calibration_for`
	// draws each module once from the measured population.

	// One measured quantity's distribution across the population.
	struct Population {
		double mean = 0.0;
		double sd   = 0.0;
		double min  = 0.0;
		double max  = 0.0;
	};

	// Current figures except the idle baseline are increments over that module's own
	// idle-emitter-off mean.
	inline constexpr Population IDLE_CURRENT_A{0.0754, 0.0101, 0.0583, 0.0850};
	inline constexpr Population IDLE_VOLTAGE_V{12.5, 0.325, 12.2, 13.0};
	inline constexpr Population LEFT_LASER_CURRENT_A{0.00372, 0.000702, 0.00227, 0.00457};
	inline constexpr Population RIGHT_LASER_CURRENT_A{0.00355, 0.000878, 0.00160, 0.00432};
	inline constexpr Population WHITE_LED_CURRENT_A{0.00511, 0.00116, 0.00282, 0.00687};
	inline constexpr Population BIG_BELT_CURRENT_A{0.106, 0.0248, 0.0641, 0.159};
	inline constexpr Population SMALL_BELT_CURRENT_A{0.108, 0.0326, 0.0518, 0.174};

	// Stored as a positive magnitude, the drop from idle voltage under maximum load.
	inline constexpr Population MAX_LOAD_VOLTAGE_SAG_V{0.0916, 0.0390, 0.0465, 0.153};

	// What a module draws, split into a baseline plus one term per actuator.
	struct Electrical {
		double idle_voltage_v = IDLE_VOLTAGE_V.mean;

		// Interpolated linearly with load between idle and full load. Not confirmed to be linear.
		double max_load_voltage_sag_v = MAX_LOAD_VOLTAGE_SAG_V.mean;

		double idle_current_a = IDLE_CURRENT_A.mean;

		// Scaled linearly with commanded speed. Not confirmed at low speed or under load.
		double big_belt_current_a   = BIG_BELT_CURRENT_A.mean;
		double small_belt_current_a = SMALL_BELT_CURRENT_A.mean;

		// Provisional placeholder, not measured.
		double servo_current_a = 0.03;

		// Scaled by summed channel intensity. Measured at full white only.
		double led_current_a_at_full_white = WHITE_LED_CURRENT_A.mean;

		double left_laser_current_a  = LEFT_LASER_CURRENT_A.mean;
		double right_laser_current_a = RIGHT_LASER_CURRENT_A.mean;
	};

	// Command points a belt speed curve is defined at, in percent. Nothing is measured between
	// 0 and 30, which is where the curve bends most.
	inline constexpr std::size_t                           BELT_CURVE_POINTS = 6;
	inline constexpr std::array<double, BELT_CURVE_POINTS> BELT_CURVE_COMMANDS{0.0, 30.0, 40.0, 60.0, 80.0, 100.0};

	// A belt's speed curve, in micrometres per second, per direction. `left` is speed under a
	// positive command, `right` under negative; both stored as positive magnitudes.
	struct BeltCurve {
		std::array<double, BELT_CURVE_POINTS> left_um_per_s{};
		std::array<double, BELT_CURVE_POINTS> right_um_per_s{};

		// Each belt has its own motor, so this is drawn and carried per belt rather than once for
		// the module. A weak motor has a modestly lower top speed and a much weaker response below
		// about half command, where it may crawl or stall.
		bool weak_motor = false;
	};

	// How a module's mechanics behave, sampled once per module and then persistent.
	struct Mechanical {
		BeltCurve big_belt;
		BeltCurve small_belt;
	};

	// How long a servo takes to travel. Entirely provisional; no usable servo measurement exists.
	struct ServoTiming {
		double full_sweep_ms         = 500.0;
		double module_offset_ms      = 0.0;
		double movement_jitter_sd_ms = 0.0;
	};

	// Physical hardware locations along the belt axes, sampled once for each module.
	struct HardwarePositions {
		// The big belt runs right endpoint=0, junction=L, left endpoint=2L.
		int64_t right_laser_position_um = 0;
		int64_t left_laser_position_um  = 0;
		// The small belt runs junction=0 to its outer endpoint=L.
		int64_t rfid_position_um = 0;
	};

	struct Calibration {
		// Provisional placeholder, not measured.
		int64_t endpoint_to_junction_um = 180000;

		// Provisional placeholder, not measured.
		int64_t encoder_counts_per_metre = 50;

		Electrical        electrical;
		Mechanical        mechanical;
		ServoTiming       servo;
		HardwarePositions hardware;

		// The population mean, with every variance term zero: one stable reference module.
		[[nodiscard]] static Calibration nominal();
	};

	// Tunables that are not per-module measurements: population priors and provisional servo timing.
	struct CalibrationConfig {
		// Fraction of unmeasured modules given a weak big-belt motor.
		double weak_motor_rate = 0.25;

		// A weak motor is not just slower: it is far more attenuated below half command than at top
		// speed.
		double weak_motor_top_speed_factor   = 0.85;
		double weak_motor_low_command_factor = 0.30;

		// Command below which the weak-motor attenuation applies in full, tapering to none at top
		// speed.
		double weak_motor_low_command_percent = 50.0;

		// Provisional; no small-belt travel time has ever been measured.
		double small_to_big_belt_speed_ratio = 1.0;

		double servo_full_sweep_ms         = 500.0;
		double servo_module_offset_sd_ms   = 50.0;
		double servo_movement_jitter_sd_ms = 50.0;

		// Lasers are fractions of the full big-belt length (2L); RFID is a fraction of the small-belt
		// length (L). Hardware position draws are untruncated normals.
		double left_laser_position_mean  = 0.9;
		double left_laser_position_sd    = 0.02;
		double right_laser_position_mean = 0.1;
		double right_laser_position_sd   = 0.02;
		double rfid_position_mean        = 0.6;
		double rfid_position_sd          = 0.1;
	};

	// Lowest belt command magnitude the hardware accepts. A belt is stopped at zero or commanded
	// between this and 100 percent; nothing in between is valid.
	inline constexpr double MIN_BELT_COMMAND_PERCENT = 25.0;

	[[nodiscard]] bool is_valid_belt_command_percent(double percent);

	// Evaluates a belt speed curve at a commanded speed (-100 to 100), interpolating linearly.
	[[nodiscard]] double belt_speed_um_per_s(const BeltCurve& curve, double percent);

	// Travel time in ticks for a commanded servo movement, never negative.
	[[nodiscard]] int64_t servo_travel_ticks(const Calibration& calibration, double from_degrees, double to_degrees,
											 uint64_t master_seed, module::ModuleId id, uint64_t movement_index);

	// That module's calibration, drawn once from the measured population and memoised. The
	// reference stays valid until the seed or override changes, which only happens at startup.
	[[nodiscard]] const Calibration& calibration_for(module::ModuleId id, uint64_t master_seed);

	// That module's calibration under the run's active seed.
	[[nodiscard]] const Calibration& calibration_for(module::ModuleId id);

	// Chooses the fleet for the run, discarding any memoised draws.
	void set_calibration_seed(uint64_t master_seed);

	// Discards the seed, any override, and memoised draws, returning to the seed-zero fleet.
	void clear_calibration_seed();

	[[nodiscard]] uint64_t active_calibration_seed();

	// Pins one calibration for every module regardless of seed. For tests that need a fixed
	// reference module, not a way to configure a real run.
	void set_calibration_override(const Calibration& calibration);

	[[nodiscard]] const CalibrationConfig& calibration_config();

	// Replaces the tunables and discards memoised draws.
	void set_calibration_config(const CalibrationConfig& config);

} // namespace world

#endif // WORLD_CALIBRATION_H
