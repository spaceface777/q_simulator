#include "calibration.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>

#include "../seed/seed.h"

namespace world {

	namespace {

		constexpr double  UM_PER_M         = 1000000.0;
		constexpr int64_t TICKS_PER_SECOND = 1000;

		// Every module's belt is drawn from this population. Modules are drawn from the same
		// distribution, so which one is slow is a property of the seed, not of the module id.

		// A curve is not sampled point by point, since independent draws could produce a nonmonotonic
		// curve. Instead three shape parameters are drawn and the curve is rebuilt from them.

		// Speed at 100 percent.
		constexpr Population BELT_TOP_SPEED_M_PER_S{0.0875, 0.00803, 0.0759, 0.0976};

		// Speed at 30 percent as a fraction of speed at 100 percent. Its spread is large: two modules
		// that finish a long run together can differ by a factor of three off the line.
		constexpr Population BELT_LOW_COMMAND_FRACTION{0.444, 0.190, 0.200, 0.660};

		// Rightward top speed divided by leftward top speed. Always above 1: every measured module ran
		// at least slightly faster rightward.
		constexpr Population BELT_DIRECTION_MULTIPLIER{1.10, 0.0960, 1.02, 1.26};

		// Where each intermediate command sits between the 30 percent fraction and 1.0, normalised by
		// top speed.
		constexpr std::array<double, BELT_CURVE_POINTS> SHAPE_POSITION{0.0, 0.0, 0.349, 0.686, 0.860, 1.0};

		// The population-mean curve, used as the nominal belt.
		constexpr std::array<double, BELT_CURVE_POINTS> NOMINAL_LEFT_M_PER_S{0.0,    0.0353, 0.0529,
																			 0.0696, 0.0786, 0.0836};
		constexpr std::array<double, BELT_CURVE_POINTS> NOMINAL_RIGHT_M_PER_S{0.0,    0.0434, 0.0595,
																			  0.0750, 0.0826, 0.0915};

		CalibrationConfig& mutable_config() {
			static CalibrationConfig config;
			return config;
		}

		std::map<std::pair<uint64_t, uint8_t>, Calibration>& cache() {
			static std::map<std::pair<uint64_t, uint8_t>, Calibration> entries;
			return entries;
		}

		// The draws of the active seed by module ID. The world asks for them several times in every tick.
		std::array<const Calibration*, 256>& active_draws() {
			static std::array<const Calibration*, 256> draws{};
			return draws;
		}

		std::optional<uint64_t>& active_seed() {
			static std::optional<uint64_t> seed;
			return seed;
		}

		std::optional<Calibration>& active_override() {
			static std::optional<Calibration> pinned;
			return pinned;
		}

		void forget_draws() {
			cache().clear();
			active_draws().fill(nullptr);
		}

		double clamp(double value, double low, double high) {
			return std::min(std::max(value, low), high);
		}

		// Next value in the stream as a uniform in [0, 1).
		double uniform01(seed::SplitMix64& stream) {
			// 53 bits, which is every bit a double can hold without rounding.
			return static_cast<double>(stream.next() >> 11) * (1.0 / 9007199254740992.0);
		}

		// A standard normal draw, by the Box-Muller transform. Needed because the min/max bounds are
		// several standard deviations out, and a tailless distribution would never reach them.
		double standard_normal(seed::SplitMix64& stream) {
			// Guard the log against a zero draw, which is possible and would be infinite.
			const double first  = std::max(uniform01(stream), 1e-12);
			const double second = uniform01(stream);
			return std::sqrt(-2.0 * std::log(first)) * std::cos(2.0 * M_PI * second);
		}

		// Draws one value from a population, redrawing until it lands in [min, max], and falls back to
		// clamping after a fixed number of attempts.
		double draw(seed::SplitMix64& stream, const Population& population) {
			constexpr int MAX_ATTEMPTS = 64;

			if (population.sd <= 0.0) {
				return clamp(population.mean, population.min, population.max);
			}

			for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
				const double value = population.mean + population.sd * standard_normal(stream);
				if (value >= population.min && value <= population.max) {
					return value;
				}
			}
			return clamp(population.mean + population.sd * standard_normal(stream), population.min, population.max);
		}

		seed::SplitMix64 stream_for(uint64_t master_seed, module::ModuleId id, const std::string& field) {
			return seed::SplitMix64(seed::derive(master_seed, "calibration/" + std::to_string(id.raw()) + "/" + field));
		}

		BeltCurve scale(const BeltCurve& curve, double factor) {
			BeltCurve scaled;
			for (std::size_t index = 0; index < BELT_CURVE_POINTS; index++) {
				scaled.left_um_per_s[index]  = curve.left_um_per_s[index] * factor;
				scaled.right_um_per_s[index] = curve.right_um_per_s[index] * factor;
			}
			scaled.weak_motor = curve.weak_motor;
			return scaled;
		}

		BeltCurve curve_from_metres(const std::array<double, BELT_CURVE_POINTS>& left,
									const std::array<double, BELT_CURVE_POINTS>& right) {
			BeltCurve curve;
			for (std::size_t index = 0; index < BELT_CURVE_POINTS; index++) {
				curve.left_um_per_s[index]  = left[index] * UM_PER_M;
				curve.right_um_per_s[index] = right[index] * UM_PER_M;
			}
			return curve;
		}

		// How much a weak motor is held back at a given command: full attenuation at and below 30
		// percent, none at and above the threshold, linear between.
		double weak_motor_attenuation(double command_percent, const CalibrationConfig& config) {
			const double full = MIN_BELT_COMMAND_PERCENT + 5.0;
			if (command_percent <= full) {
				return config.weak_motor_low_command_factor;
			}
			if (command_percent >= config.weak_motor_low_command_percent) {
				return 1.0;
			}
			const double span     = config.weak_motor_low_command_percent - full;
			const double position = (command_percent - full) / span;
			return config.weak_motor_low_command_factor + (1.0 - config.weak_motor_low_command_factor) * position;
		}

		// The three parameters a belt curve is rebuilt from. Big and small belts are different
		// motors, but neither's shape has ever been measured on its own, so both are drawn once
		// per module and shared; each belt then applies its own independently-sampled weak status
		// on top of this shared shape.
		struct CurveShape {
			double left_top;
			double low_fraction;
			double direction;
		};

		CurveShape sampled_shape(seed::SplitMix64& stream) {
			return CurveShape{
				draw(stream, BELT_TOP_SPEED_M_PER_S),
				draw(stream, BELT_LOW_COMMAND_FRACTION),
				draw(stream, BELT_DIRECTION_MULTIPLIER),
			};
		}

		// Rebuilds a plausible belt curve from a shape and this belt's own motor's weak status.
		BeltCurve curve_from_shape(const CurveShape& shape, bool weak, const CalibrationConfig& config) {
			double left_top = shape.left_top;
			if (weak) {
				left_top *= config.weak_motor_top_speed_factor;
			}

			// If the faster direction would exceed the measured ceiling, scale both directions down
			// together so the left/right asymmetry is preserved.
			const double ceiling = BELT_TOP_SPEED_M_PER_S.max * (weak ? config.weak_motor_top_speed_factor : 1.0);
			if (left_top * shape.direction > ceiling) {
				left_top = ceiling / shape.direction;
			}
			const double right_top = left_top * shape.direction;

			BeltCurve curve;
			for (std::size_t index = 0; index < BELT_CURVE_POINTS; index++) {
				const double command = BELT_CURVE_COMMANDS[index];
				double       point =
					command == 0.0 ? 0.0 : shape.low_fraction + SHAPE_POSITION[index] * (1.0 - shape.low_fraction);
				if (weak) {
					point *= weak_motor_attenuation(command, config);
				}
				curve.left_um_per_s[index]  = left_top * point * UM_PER_M;
				curve.right_um_per_s[index] = right_top * point * UM_PER_M;
			}
			curve.weak_motor = weak;
			return curve;
		}

		Calibration sample(module::ModuleId id, uint64_t master_seed) {
			const CalibrationConfig& config      = calibration_config();
			Calibration              calibration = Calibration::nominal();

			{
				seed::SplitMix64 stream                = stream_for(master_seed, id, "electrical");
				Electrical&      electrical            = calibration.electrical;
				electrical.idle_voltage_v              = draw(stream, IDLE_VOLTAGE_V);
				electrical.idle_current_a              = draw(stream, IDLE_CURRENT_A);
				electrical.max_load_voltage_sag_v      = draw(stream, MAX_LOAD_VOLTAGE_SAG_V);
				electrical.big_belt_current_a          = draw(stream, BIG_BELT_CURRENT_A);
				electrical.small_belt_current_a        = draw(stream, SMALL_BELT_CURRENT_A);
				electrical.led_current_a_at_full_white = draw(stream, WHITE_LED_CURRENT_A);
				// Drawn separately per side: the two have different means and spreads.
				electrical.left_laser_current_a  = draw(stream, LEFT_LASER_CURRENT_A);
				electrical.right_laser_current_a = draw(stream, RIGHT_LASER_CURRENT_A);
			}

			{
				seed::SplitMix64 stream     = stream_for(master_seed, id, "mechanical");
				const CurveShape shape      = sampled_shape(stream);
				const bool       big_weak   = uniform01(stream) < config.weak_motor_rate;
				const bool       small_weak = uniform01(stream) < config.weak_motor_rate;

				calibration.mechanical.big_belt = curve_from_shape(shape, big_weak, config);

				// The small belt has never been timed, so its underlying strength and directionality
				// are borrowed from the same draw as the big belt's, scaled by one provisional ratio.
				// But it is a distinct motor, so whether it happens to be weak is sampled
				// independently rather than copied from the big belt.
				calibration.mechanical.small_belt =
					scale(curve_from_shape(shape, small_weak, config), config.small_to_big_belt_speed_ratio);
			}

			{
				seed::SplitMix64 stream                 = stream_for(master_seed, id, "servo");
				calibration.servo.full_sweep_ms         = config.servo_full_sweep_ms;
				calibration.servo.module_offset_ms      = config.servo_module_offset_sd_ms * standard_normal(stream);
				calibration.servo.movement_jitter_sd_ms = config.servo_movement_jitter_sd_ms;
			}

			{
				const int64_t    leg_length_um               = calibration.endpoint_to_junction_um;
				const double     big_belt_length_um          = static_cast<double>(2 * leg_length_um);
				const double     small_belt_length_um        = static_cast<double>(leg_length_um);
				seed::SplitMix64 stream                      = stream_for(master_seed, id, "hardware_positions");
				calibration.hardware.right_laser_position_um = static_cast<int64_t>(
					std::llround(big_belt_length_um * (config.right_laser_position_mean +
													   config.right_laser_position_sd * standard_normal(stream))));
				calibration.hardware.left_laser_position_um = static_cast<int64_t>(
					std::llround(big_belt_length_um * (config.left_laser_position_mean +
													   config.left_laser_position_sd * standard_normal(stream))));
				calibration.hardware.rfid_position_um = static_cast<int64_t>(
					std::llround(small_belt_length_um *
								 (config.rfid_position_mean + config.rfid_position_sd * standard_normal(stream))));
			}

			return calibration;
		}

	} // namespace

	Calibration Calibration::nominal() {
		const CalibrationConfig& config = calibration_config();

		Calibration calibration;
		calibration.mechanical.big_belt = curve_from_metres(NOMINAL_LEFT_M_PER_S, NOMINAL_RIGHT_M_PER_S);
		calibration.mechanical.small_belt =
			scale(calibration.mechanical.big_belt, config.small_to_big_belt_speed_ratio);
		calibration.servo.full_sweep_ms         = config.servo_full_sweep_ms;
		calibration.servo.module_offset_ms      = 0.0;
		calibration.servo.movement_jitter_sd_ms = 0.0;
		const double big_belt_length_um         = static_cast<double>(2 * calibration.endpoint_to_junction_um);
		const double small_belt_length_um       = static_cast<double>(calibration.endpoint_to_junction_um);
		calibration.hardware.right_laser_position_um =
			static_cast<int64_t>(std::llround(big_belt_length_um * config.right_laser_position_mean));
		calibration.hardware.left_laser_position_um =
			static_cast<int64_t>(std::llround(big_belt_length_um * config.left_laser_position_mean));
		calibration.hardware.rfid_position_um =
			static_cast<int64_t>(std::llround(small_belt_length_um * config.rfid_position_mean));
		return calibration;
	}

	bool is_valid_belt_command_percent(double percent) {
		if (!std::isfinite(percent)) {
			return false;
		}
		const double magnitude = std::fabs(percent);
		if (magnitude == 0.0) {
			return true;
		}
		return magnitude >= MIN_BELT_COMMAND_PERCENT && magnitude <= 100.0;
	}

	double belt_speed_um_per_s(const BeltCurve& curve, double percent) {
		if (!std::isfinite(percent) || percent == 0.0) {
			return 0.0;
		}

		const bool                                   leftward  = percent > 0.0;
		const std::array<double, BELT_CURVE_POINTS>& points    = leftward ? curve.left_um_per_s : curve.right_um_per_s;
		const double                                 magnitude = std::min(std::fabs(percent), 100.0);

		double speed = points[BELT_CURVE_POINTS - 1];
		for (std::size_t index = 1; index < BELT_CURVE_POINTS; index++) {
			if (magnitude <= BELT_CURVE_COMMANDS[index]) {
				const double low_command = BELT_CURVE_COMMANDS[index - 1];
				const double span        = BELT_CURVE_COMMANDS[index] - low_command;
				const double position    = span <= 0.0 ? 0.0 : (magnitude - low_command) / span;
				speed                    = points[index - 1] + position * (points[index] - points[index - 1]);
				break;
			}
		}

		return leftward ? speed : -speed;
	}

	int64_t servo_travel_ticks(const Calibration& calibration, double from_degrees, double to_degrees,
							   uint64_t master_seed, module::ModuleId id, uint64_t movement_index) {
		constexpr double FULL_SWEEP_DEGREES = 180.0;

		const double fraction = std::fabs(to_degrees - from_degrees) / FULL_SWEEP_DEGREES;
		if (fraction <= 0.0) {
			return 0;
		}

		// The module's own offset is part of its rate rather than a fixed cost, so a persistently
		// slow servo is slow over a short movement too.
		const double sweep_ms    = std::max(calibration.servo.full_sweep_ms + calibration.servo.module_offset_ms, 1.0);
		double       duration_ms = fraction * sweep_ms;

		if (calibration.servo.movement_jitter_sd_ms > 0.0) {
			seed::SplitMix64 stream(seed::derive(master_seed, "servo/" + std::to_string(id.raw()) + "/movement/" +
																  std::to_string(movement_index)));
			duration_ms += fraction * calibration.servo.movement_jitter_sd_ms * standard_normal(stream);
		}

		// A movement cannot take less than no time however the jitter falls.
		const double ticks = std::round(duration_ms * static_cast<double>(TICKS_PER_SECOND) / 1000.0);
		return ticks <= 0.0 ? 0 : static_cast<int64_t>(ticks);
	}

	const Calibration& calibration_for(module::ModuleId id, uint64_t master_seed) {
		const std::pair<uint64_t, uint8_t> key{master_seed, id.raw()};

		const auto found = cache().find(key);
		if (found != cache().end()) {
			return found->second;
		}

		return cache().emplace(key, sample(id, master_seed)).first->second;
	}

	const Calibration& calibration_for(module::ModuleId id) {
		if (active_override()) {
			return *active_override();
		}
		const Calibration*& draw = active_draws()[id.raw()];
		if (!draw) draw = &calibration_for(id, active_seed().value_or(0));
		return *draw;
	}

	void set_calibration_seed(uint64_t master_seed) {
		active_override().reset();
		active_seed() = master_seed;
		forget_draws();
	}

	void clear_calibration_seed() {
		active_override().reset();
		active_seed().reset();
		forget_draws();
	}

	uint64_t active_calibration_seed() {
		return active_seed().value_or(0);
	}

	void set_calibration_override(const Calibration& calibration) {
		active_override() = calibration;
	}

	const CalibrationConfig& calibration_config() {
		return mutable_config();
	}

	void set_calibration_config(const CalibrationConfig& config) {
		mutable_config() = config;
		forget_draws();
	}

} // namespace world
