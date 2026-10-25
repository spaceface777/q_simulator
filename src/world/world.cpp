#include "world.h"

#include <bitset>
#include <cmath>
#include <stdexcept>

#include "../seed/seed.h"
#include "../settings.h"

namespace world {

	namespace {

		// Belt speeds arrive as percentages with a fraction, carried at a thousandth of a percent.
		constexpr int64_t PERCENT_SCALE = 1000;

		constexpr int64_t TICKS_PER_SECOND = 1000;

		// Firmware constant, not a measurement.
		constexpr double INA226_R_SHUNT_OHM = 0.002;

		// The INA226's fixed hardware resolution on its two ADC channels, independent of R_SHUNT.
		constexpr double INA226_BUS_LSB_V   = 0.00125;
		constexpr double INA226_SHUNT_LSB_V = 0.0000025;

		// Rounds to the nearest multiple of an ADC's least significant bit.
		double quantize(double value, double lsb) {
			return std::round(value / lsb) * lsb;
		}

		enum class ServoRoute : uint8_t {
			STRAIGHT,
			RIGHT_DIVERSION,
			BLOCK,
			LEFT_DIVERSION,
		};

		double smootherstep(double position) {
			return position * position * position * (position * ((6.0 * position) - 15.0) + 10.0);
		}

		// One uniform draw per tub/module/junction encounter. Reusing it while the tub waits makes a
		// moving servo cross one stable sampled threshold instead of giving a held tub another chance
		// to escape on every millisecond tick.
		double junction_route_draw(const Tub& tub) {
			const std::string label = "junction-route/" + std::to_string(tub.module.raw()) + "/" +
									  std::to_string(tub.id) + "/" + std::to_string(tub.junction_encounter);
			seed::SplitMix64  stream(seed::derive(active_calibration_seed(), label));
			return static_cast<double>(stream.next() >> 11) * (1.0 / 9007199254740992.0);
		}

		ServoRoute blend_route(const Tub& tub, double angle, double lower_angle, double upper_angle,
							   ServoRoute lower_route, ServoRoute upper_route) {
			const double position          = (angle - lower_angle) / (upper_angle - lower_angle);
			const double upper_probability = smootherstep(position);
			return junction_route_draw(tub) < upper_probability ? upper_route : lower_route;
		}

		// Certain physical regions are separated by smooth seeded transition regions. The wider
		// 0-20 and 160-180 neutral bands deliberately include the canonical 0-10 and 170-180 end-stop
		// ranges plus their requested effectively-neutral margins.
		ServoRoute servo_route_for(const Tub& tub, double angle) {
			if (angle <= 20.0) return ServoRoute::STRAIGHT;
			if (angle < 50.0) {
				return blend_route(tub, angle, 20.0, 50.0, ServoRoute::STRAIGHT, ServoRoute::RIGHT_DIVERSION);
			}
			if (angle <= 70.0) return ServoRoute::RIGHT_DIVERSION;
			if (angle < 80.0) {
				return blend_route(tub, angle, 70.0, 80.0, ServoRoute::RIGHT_DIVERSION, ServoRoute::BLOCK);
			}
			if (angle <= 100.0) return ServoRoute::BLOCK;
			if (angle < 110.0) {
				return blend_route(tub, angle, 100.0, 110.0, ServoRoute::BLOCK, ServoRoute::LEFT_DIVERSION);
			}
			if (angle <= 130.0) return ServoRoute::LEFT_DIVERSION;
			if (angle < 160.0) {
				return blend_route(tub, angle, 130.0, 160.0, ServoRoute::LEFT_DIVERSION, ServoRoute::STRAIGHT);
			}
			return ServoRoute::STRAIGHT;
		}

		// The belt and physical endpoint offset used by inter-module hand-offs and endpoint occupancy.
		std::pair<Belt, int64_t> endpoint_placement(Position position, int64_t leg_length_um) {
			switch (position) {
			case Position::ENTRY:       return {Belt::SMALL, leg_length_um};
			case Position::LEFT_LASER:  return {Belt::BIG, 2 * leg_length_um};
			case Position::RIGHT_LASER: return {Belt::BIG, 0};
			case Position::CENTER:      break;
			}
			return {Belt::BIG, leg_length_um};
		}

		// The calibrated sensor coordinate placed by the scenario/API position names.
		std::pair<Belt, int64_t> sensor_placement(Position position, const Calibration& calibration) {
			switch (position) {
			case Position::ENTRY:       return {Belt::SMALL, calibration.hardware.rfid_position_um};
			case Position::LEFT_LASER:  return {Belt::BIG, calibration.hardware.left_laser_position_um};
			case Position::RIGHT_LASER: return {Belt::BIG, calibration.hardware.right_laser_position_um};
			case Position::CENTER:      break;
			}
			return {Belt::BIG, calibration.endpoint_to_junction_um};
		}

		bool at_junction(const Tub& tub, int64_t leg_length_um) {
			if (tub.belt == Belt::BIG) {
				return tub.position_um == leg_length_um;
			}
			return tub.position_um == 0;
		}

		bool at_fixed_point(const Tub& tub, int64_t leg_length_um) {
			if (tub.belt == Belt::BIG) {
				return tub.position_um == 0 || tub.position_um == leg_length_um || tub.position_um == 2 * leg_length_um;
			}
			return tub.position_um == 0 || tub.position_um == leg_length_um;
		}

		// The port a tub leaves through, if it is at an endpoint with the belt still driving it outward.
		std::optional<ModuleLocation> exit_port(const Tub& tub, const module::Module& module, int64_t leg_length_um) {
			if (tub.belt == Belt::BIG) {
				if (tub.position_um == 2 * leg_length_um && module.big_belt_speed().raw() > 0) {
					return ModuleLocation::LEFT;
				}
				if (tub.position_um == 0 && module.big_belt_speed().raw() < 0) {
					return ModuleLocation::RIGHT;
				}
				return {};
			}
			if (tub.position_um == leg_length_um && module.small_belt_speed().raw() > 0) {
				return ModuleLocation::MIDDLE;
			}
			return {};
		}

		std::optional<module::ModuleId> neighbour(module::ModuleId id, ModuleLocation port) {
			const settings::ModuleSettings module_settings = settings::get_module_settings(id);
			switch (port) {
			case ModuleLocation::LEFT:   return module_settings.left;
			case ModuleLocation::MIDDLE: return module_settings.middle;
			case ModuleLocation::RIGHT:  return module_settings.right;
			}
			return {};
		}

		// Where a tub arriving from a given module enters: the endpoint whose port faces back the way
		// it came.
		std::optional<Position> entry_position(module::ModuleId destination, module::ModuleId source) {
			const settings::ModuleSettings module_settings = settings::get_module_settings(destination);
			if (module_settings.left && *module_settings.left == source) {
				return Position::LEFT_LASER;
			}
			if (module_settings.middle && *module_settings.middle == source) {
				return Position::ENTRY;
			}
			if (module_settings.right && *module_settings.right == source) {
				return Position::RIGHT_LASER;
			}
			return {};
		}

		// Whether a module's belts would carry a tub inward from the given endpoint. A tub is not
		// handed onto a belt that would push it straight back.
		bool belt_accepts(const module::Module& module, Position position) {
			switch (position) {
			case Position::LEFT_LASER:  return module.big_belt_speed().raw() < 0;
			case Position::RIGHT_LASER: return module.big_belt_speed().raw() > 0;
			case Position::ENTRY:       return module.small_belt_speed().raw() < 0;
			case Position::CENTER:      break;
			}
			return false;
		}

		double clamp01(double value) {
			if (value < 0.0) {
				return 0.0;
			}
			if (value > 1.0) {
				return 1.0;
			}
			return value;
		}

	} // namespace

	int64_t tub_radius_um(const Calibration& calibration) {
		const double big_belt_length_um = 2.0 * static_cast<double>(calibration.endpoint_to_junction_um);
		return static_cast<int64_t>(std::llround(big_belt_length_um * TUB_RADIUS_BIG_BELT_FRACTION));
	}

	int64_t rfid_half_length_um(const Calibration& calibration) {
		return static_cast<int64_t>(
			std::llround(static_cast<double>(calibration.endpoint_to_junction_um) * RFID_HALF_SMALL_BELT_FRACTION));
	}

	int64_t belt_step_um(double um_per_s, int64_t& carry) {
		// Carried at a thousandth of a micrometre per second, so a belt slower than one micrometre
		// per tick still accumulates travel exactly instead of rounding to a standstill.
		const int64_t rate_milli = static_cast<int64_t>(std::llround(um_per_s * static_cast<double>(PERCENT_SCALE)));
		if (rate_milli == 0) {
			carry = 0;
			return 0;
		}

		const int64_t denominator = PERCENT_SCALE * TICKS_PER_SECOND;
		const int64_t numerator   = carry + rate_milli;
		const int64_t step        = numerator / denominator;
		carry                     = numerator % denominator;
		return step;
	}

	int64_t belt_step_um(double speed_percent, const BeltCurve& curve, int64_t& carry) {
		return belt_step_um(belt_speed_um_per_s(curve, speed_percent), carry);
	}

	void configure_calibration_from_settings() {
		CalibrationConfig config;
		config.weak_motor_rate               = settings::get_setting<double>("weak_motor_rate");
		config.small_to_big_belt_speed_ratio = settings::get_setting<double>("small_to_big_belt_speed_ratio");
		config.servo_full_sweep_ms           = settings::get_setting<double>("servo_full_sweep_ms");
		config.servo_module_offset_sd_ms     = settings::get_setting<double>("servo_module_offset_sd_ms");
		config.servo_movement_jitter_sd_ms   = settings::get_setting<double>("servo_movement_jitter_sd_ms");
		config.left_laser_position_mean      = settings::get_setting<double>("left_laser_position_mean");
		config.left_laser_position_sd        = settings::get_setting<double>("left_laser_position_sd");
		config.right_laser_position_mean     = settings::get_setting<double>("right_laser_position_mean");
		config.right_laser_position_sd       = settings::get_setting<double>("right_laser_position_sd");
		config.rfid_position_mean            = settings::get_setting<double>("rfid_position_mean");
		config.rfid_position_sd              = settings::get_setting<double>("rfid_position_sd");
		set_calibration_config(config);
	}

	void command_servo(module::Module& module, module::ServoAngle target) {
		const Calibration& calibration = calibration_for(module.id());
		const int64_t      travel =
			servo_travel_ticks(calibration, module.servo_actual_angle().raw(), target.raw(), active_calibration_seed(),
							   module.id(), module.servo_movement_count());
		module.command_servo_angle(target, travel);
	}

	void State::add_module(module::ModuleId id) {
		modules_.emplace(id.raw(), module::Module(id.raw()));
	}

	module::Module* State::module(module::ModuleId id) {
		const auto found = modules_.find(id.raw());
		return found == modules_.end() ? nullptr : &found->second;
	}

	const module::Module* State::module(module::ModuleId id) const {
		const auto found = modules_.find(id.raw());
		return found == modules_.end() ? nullptr : &found->second;
	}

	void State::place(uint8_t id, module::ModuleId module_id, Position position, const RfidUid& uid,
					  const RfidPayload& payload, module::Changes* changes) {
		if (tubs_.count(id) != 0) {
			throw std::invalid_argument("Tub " + std::to_string(id) + " is already in the world");
		}
		if (module(module_id) == nullptr) {
			throw std::invalid_argument("Cannot place tub " + std::to_string(id) + " on unknown module " +
										std::to_string(module_id.raw()));
		}

		const Calibration&             calibration = calibration_for(module_id);
		const std::pair<Belt, int64_t> placed      = sensor_placement(position, calibration);

		Tub tub;
		tub.id          = id;
		tub.module      = module_id;
		tub.belt        = placed.first;
		tub.position_um = placed.second;
		tub.uid         = uid;
		tub.payload     = payload;

		removal_reasons_.erase(id);
		tubs_.emplace(id, std::move(tub));
		update_sensors(changes);
	}

	void State::remove(uint8_t id, TubRemovalReason reason, module::Changes* changes) {
		if (tubs_.erase(id) != 0) {
			removal_reasons_[id] = reason;
		}
		update_sensors(changes);
	}

	std::optional<TubRemovalReason> State::removal_reason(uint8_t id) const {
		const auto found = removal_reasons_.find(id);
		return found == removal_reasons_.end() ? std::optional<TubRemovalReason>{} : found->second;
	}

	bool State::write_rfid_page(module::ModuleId id, std::size_t page, const RfidPage& value) {
		if (page >= RFID_PAYLOAD_PAGE_COUNT) {
			return false;
		}

		const Tub* selected = tub_at_reader(id);
		if (selected == nullptr) return false;
		tubs_.at(selected->id).payload[page] = value;
		return true;
	}

	void State::clear() {
		tubs_.clear();
		removal_reasons_.clear();
	}

	void State::advance_tick(module::Changes* changes) {
		// std::map iterates in ascending key order, so two tubs contending for the same endpoint
		// always resolve the same way rather than in whatever order they were created.
		for (auto iterator = tubs_.begin(); iterator != tubs_.end();) {
			const auto current = iterator++;
			Tub&       tub     = current->second;

			const module::Module* module_ptr = module(tub.module);
			if (module_ptr == nullptr) {
				tub.force_eject = 0;
				continue;
			}
			const module::Module& module        = *module_ptr;
			const Calibration&    calibration   = calibration_for(tub.module);
			const int64_t         leg_length_um = calibration.endpoint_to_junction_um;
			if (leg_length_um <= 0) {
				tub.force_eject = 0;
				continue;
			}

			const std::optional<ModuleLocation> port = exit_port(tub, module, leg_length_um);
			if (port && !neighbour(tub.module, *port)) {
				tub.force_eject++;
				if (tub.force_eject >= TUB_OOB_YEET_MS) {
					removal_reasons_[tub.id] = TubRemovalReason::YEETED;
					tubs_.erase(current);
					continue;
				}
			} else {
				tub.force_eject = 0;
			}

			if (at_junction(tub, leg_length_um) && !resolve_junction(tub, leg_length_um)) {
				continue;
			}

			const double speed =
				tub.belt == Belt::BIG ? module.big_belt_speed().raw() : module.small_belt_speed().raw();
			const BeltCurve& curve =
				tub.belt == Belt::BIG ? calibration.mechanical.big_belt : calibration.mechanical.small_belt;
			const int64_t step = belt_step_um(speed, curve, tub.carry);
			if (step == 0) {
				continue;
			}

			move(tub, leg_length_um, step);
		}

		// Belts and the servo arm advance from the commanded speeds rather than from the tubs, since
		// they move whether or not anything is riding on or waiting for them.
		for (auto& entry : modules_) {
			module::Module&    hardware    = entry.second;
			const Calibration& calibration = calibration_for(hardware.id());
			const int64_t      big = belt_step_um(hardware.big_belt_speed().raw(), calibration.mechanical.big_belt,
												  hardware.big_belt_travel_carry());
			const int64_t small    = belt_step_um(hardware.small_belt_speed().raw(), calibration.mechanical.small_belt,
												  hardware.small_belt_travel_carry());
			hardware.advance_belt_travel(big, small);
			if (changes && hardware.is_servo_moving()) changes->set(hardware.id().raw());
			hardware.advance_servo_travel();
		}

		update_sensors(changes);
	}

	int64_t encoder_count(int64_t travel_um, const Calibration& calibration) {
		constexpr int64_t UM_PER_METRE = 1000000;
		return travel_um * calibration.encoder_counts_per_metre / UM_PER_METRE;
	}

	double encoder_frequency_hz(double speed_percent, const BeltCurve& curve, const Calibration& calibration) {
		constexpr double UM_PER_METRE      = 1000000.0;
		const double     metres_per_second = std::fabs(belt_speed_um_per_s(curve, speed_percent)) / UM_PER_METRE;
		return metres_per_second * static_cast<double>(calibration.encoder_counts_per_metre);
	}

	Electrics electrics_of(const module::Module& module, const Calibration& calibration) {
		const Electrical& draw = calibration.electrical;

		double load = 0.0;
		load += std::fabs(module.big_belt_speed().raw()) / 100.0 * draw.big_belt_current_a;
		load += std::fabs(module.small_belt_speed().raw()) / 100.0 * draw.small_belt_current_a;

		// The servo draws while travelling and while holding at any angle other than rest.
		if (module.is_servo_moving() || module.servo_actual_angle().raw() != 0.0) {
			load += draw.servo_current_a;
		}

		// An RGB LED draws per lit channel, summed against a full-white reference.
		const uint32_t colour = module.led_color().raw();
		const double channels = static_cast<double>((colour >> 16) & 0xFF) + static_cast<double>((colour >> 8) & 0xFF) +
								static_cast<double>(colour & 0xFF);
		load += channels / (3.0 * 255.0) * draw.led_current_a_at_full_white;

		load += (module.is_left_laser_enabled() ? draw.left_laser_current_a : 0.0);
		load += (module.is_right_laser_enabled() ? draw.right_laser_current_a : 0.0);

		const double current = draw.idle_current_a + load;

		// The supply sags with load, scaled by load as a fraction of the maximum-load configuration.
		const double max_load      = draw.big_belt_current_a + draw.small_belt_current_a + draw.servo_current_a +
									 draw.led_current_a_at_full_white + draw.left_laser_current_a +
									 draw.right_laser_current_a;
		const double load_fraction = max_load > 0.0 ? clamp01(load / max_load) : 0.0;
		const double supply_v      = draw.idle_voltage_v - load_fraction * draw.max_load_voltage_sag_v;

		// Derived as the INA226 does: bus plus shunt drop for voltage, bus times current for power.
		// The two channels are quantized independently to preserve the shunt channel's resolution.
		const double bus_v   = quantize(supply_v, INA226_BUS_LSB_V);
		const double shunt_v = quantize(current * INA226_R_SHUNT_OHM, INA226_SHUNT_LSB_V);

		Electrics electrics;
		electrics.current_a = shunt_v / INA226_R_SHUNT_OHM;
		electrics.voltage_v = bus_v + shunt_v;
		electrics.power_w   = bus_v * electrics.current_a;
		return electrics;
	}

	bool State::resolve_junction(Tub& tub, int64_t leg_length_um) {
		const module::Module* module_ptr = module(tub.module);
		if (module_ptr == nullptr) {
			return false;
		}
		const module::Module& module = *module_ptr;

		const auto hold = [&tub, leg_length_um]() {
			tub.position_um = tub.belt == Belt::BIG ? leg_length_um : 0;
			tub.carry       = 0;
		};

		const auto move_on_big = [&tub, leg_length_um]() {
			tub.belt        = Belt::BIG;
			tub.position_um = leg_length_um;
			tub.carry       = 0;
			return true;
		};

		const auto move_on_small = [&tub]() {
			tub.belt        = Belt::SMALL;
			tub.position_um = 0;
			tub.carry       = 0;
			return true;
		};

		const double     big_speed   = module.big_belt_speed().raw();
		const double     small_speed = module.small_belt_speed().raw();
		const ServoRoute route       = servo_route_for(tub, module.servo_actual_angle().raw());

		// Once a tub is on the small branch, running that belt outward carries it away from the
		// junction regardless of where the servo moves afterwards.
		if (tub.belt == Belt::SMALL && small_speed > 0.0) {
			return move_on_small();
		}

		if (tub.belt == Belt::BIG) {
			if (route == ServoRoute::STRAIGHT) {
				if (big_speed != 0.0) return move_on_big();
				hold();
				return false;
			}

			const bool from_right = big_speed > 0.0;
			const bool from_left  = big_speed < 0.0;
			const bool selected =
				(from_right && route == ServoRoute::RIGHT_DIVERSION) ||
				(from_left && route == ServoRoute::LEFT_DIVERSION) ||
				(big_speed == 0.0 && (route == ServoRoute::RIGHT_DIVERSION || route == ServoRoute::LEFT_DIVERSION));
			if (selected && small_speed > 0.0) {
				return move_on_small();
			}

			// The central block, an opposite-side diversion, or a selected diversion whose small belt
			// cannot receive the tub all physically obstruct the straight big-belt path.
			hold();
			return false;
		}

		// The small belt pushes an inbound tub onto the centre of the big belt unless the servo is
		// physically blocking the junction. Once transferred, the big belt independently decides
		// whether and in which direction the tub moves.
		if (small_speed < 0.0 && route != ServoRoute::BLOCK) {
			return move_on_big();
		}

		hold();
		return false;
	}

	void State::move(Tub& tub, int64_t leg_length_um, int64_t step_um) {
		const int64_t both_legs = 2 * leg_length_um;
		const int64_t target    = tub.position_um + step_um;

		if (tub.belt == Belt::BIG) {
			if (step_um > 0) {
				// A tub reaching the junction stops there for the tick. The following tick decides
				// where it goes, which keeps belt hand-offs free of cross-belt conversion.
				if (tub.position_um < leg_length_um && target >= leg_length_um) {
					tub.position_um = leg_length_um;
					tub.junction_encounter++;
					return;
				}
				if (target >= both_legs) {
					tub.position_um = both_legs;
					hand_off(tub, leg_length_um);
					return;
				}
				tub.position_um = target;
				return;
			}

			if (tub.position_um > leg_length_um && target <= leg_length_um) {
				tub.position_um = leg_length_um;
				tub.junction_encounter++;
				return;
			}
			if (target <= 0) {
				tub.position_um = 0;
				hand_off(tub, leg_length_um);
				return;
			}
			tub.position_um = target;
			return;
		}

		if (step_um > 0) {
			if (target >= leg_length_um) {
				tub.position_um = leg_length_um;
				hand_off(tub, leg_length_um);
				return;
			}
			tub.position_um = target;
			return;
		}

		if (target <= 0) {
			tub.position_um = 0;
			tub.junction_encounter++;
			return;
		}
		tub.position_um = target;
	}

	void State::hand_off(Tub& tub, int64_t leg_length_um) {
		const module::Module* origin = module(tub.module);
		if (origin == nullptr) {
			return;
		}

		const std::optional<ModuleLocation> port = exit_port(tub, *origin, leg_length_um);
		if (!port) {
			return;
		}

		const std::optional<module::ModuleId> destination = neighbour(tub.module, *port);
		if (!destination) {
			return;
		}

		const std::optional<Position> arrival = entry_position(*destination, tub.module);
		if (!arrival || !can_receive(*destination, *arrival)) {
			return;
		}

		const std::pair<Belt, int64_t> placed =
			endpoint_placement(*arrival, calibration_for(*destination).endpoint_to_junction_um);
		tub.module      = *destination;
		tub.belt        = placed.first;
		tub.position_um = placed.second;
		tub.carry       = 0;
	}

	bool State::can_receive(module::ModuleId id, Position position) const {
		const module::Module* destination = module(id);
		if (destination == nullptr) {
			return false;
		}
		if (!belt_accepts(*destination, position)) {
			return false;
		}

		const int64_t                  leg_length_um = calibration_for(id).endpoint_to_junction_um;
		const std::pair<Belt, int64_t> occupied      = endpoint_placement(position, leg_length_um);
		for (const auto& entry : tubs_) {
			const Tub& other = entry.second;
			if (other.module == id && other.belt == occupied.first && other.position_um == occupied.second) {
				return false;
			}
		}
		return true;
	}

	const Tub* State::tub_at_reader(module::ModuleId id) const {
		const Calibration& calibration     = calibration_for(id);
		const int64_t      reader_position = calibration.hardware.rfid_position_um;
		const int64_t      half_length_um  = rfid_half_length_um(calibration);
		for (const auto& entry : tubs_) {
			const Tub& tub = entry.second;
			if (tub.module == id && tub.belt == Belt::SMALL &&
				std::llabs(tub.position_um - reader_position) <= half_length_um) {
				return &tub;
			}
		}
		return nullptr;
	}

	void State::update_sensors(module::Changes* changes) {
		std::bitset<256> big_tubs, small_tubs;
		for (const auto& [id, tub] : tubs_) (tub.belt == Belt::BIG ? big_tubs : small_tubs)[tub.module.raw()] = true;
		for (auto& entry : modules_) {
			module::Module* module = &entry.second;

			bool left_blocked  = false;
			bool right_blocked = false;

			if (big_tubs[module->id().raw()]) {
				const Calibration& calibration = calibration_for(module->id());
				const int64_t      tub_radius  = tub_radius_um(calibration);
				for (const auto& entry : tubs_) {
					const Tub& tub = entry.second;
					if (!(tub.module == module->id())) {
						continue;
					}

					if (tub.belt == Belt::BIG) {
						// A beam is a line at its calibrated coordinate; the tub blocks it when any
						// part of its physical extent intersects that line.
						if (std::llabs(tub.position_um - calibration.hardware.left_laser_position_um) <= tub_radius) {
							left_blocked = true;
						}
						if (std::llabs(tub.position_um - calibration.hardware.right_laser_position_um) <= tub_radius) {
							right_blocked = true;
						}
					}
				}
			}

			const Tub* at_reader = small_tubs[module->id().raw()] ? tub_at_reader(module->id()) : nullptr;
			if (changes && (module->is_left_laser_blocked() != left_blocked ||
							module->is_right_laser_blocked() != right_blocked || at_reader || module->is_rfid_active()))
				changes->set(module->id().raw());
			module->set_left_laser_blocked(left_blocked);
			module->set_right_laser_blocked(right_blocked);
			if (at_reader != nullptr) {
				module->set_rfid_tag(at_reader->uid, at_reader->payload);
			} else {
				module->clear_rfid_tag();
			}
		}
	}

	const Tub* State::tub(uint8_t id) const {
		const auto found = tubs_.find(id);
		return found == tubs_.end() ? nullptr : &found->second;
	}

	std::optional<Position> State::position_of(uint8_t id) const {
		const Tub* tub = this->tub(id);
		if (tub == nullptr) {
			return {};
		}
		return position_of(*tub);
	}

	std::optional<Position> State::position_of(const Tub& tub) const {
		const Calibration& calibration = calibration_for(tub.module);

		// Where the tub is, not where it is heading. Laser occupancy is tub/beam intersection.
		if (tub.belt == Belt::BIG) {
			const int64_t radius_um = tub_radius_um(calibration);
			if (std::llabs(tub.position_um - calibration.hardware.right_laser_position_um) <= radius_um) {
				return Position::RIGHT_LASER;
			}
			if (std::llabs(tub.position_um - calibration.hardware.left_laser_position_um) <= radius_um) {
				return Position::LEFT_LASER;
			}
			return Position::CENTER;
		}

		if (std::llabs(tub.position_um - calibration.hardware.rfid_position_um) <= rfid_half_length_um(calibration)) {
			return Position::ENTRY;
		}
		return Position::CENTER;
	}

	Movement State::movement_of(uint8_t id) const {
		Movement movement;

		const auto found = tubs_.find(id);
		if (found == tubs_.end()) {
			return movement;
		}
		const Tub& tub  = found->second;
		movement.module = tub.module;

		const module::Module* module_ptr = module(tub.module);
		if (module_ptr == nullptr) {
			return movement;
		}
		const module::Module& module = *module_ptr;

		const int64_t leg_length_um = calibration_for(tub.module).endpoint_to_junction_um;
		const double  speed = tub.belt == Belt::BIG ? module.big_belt_speed().raw() : module.small_belt_speed().raw();
		if (speed == 0.0 || at_fixed_point(tub, leg_length_um)) {
			return movement;
		}

		const double leg = static_cast<double>(leg_length_um);
		if (tub.belt == Belt::BIG) {
			if (speed > 0) {
				if (tub.position_um < leg_length_um) {
					movement.from     = Position::RIGHT_LASER;
					movement.to       = Position::CENTER;
					movement.progress = static_cast<double>(tub.position_um) / leg;
				} else {
					movement.from     = Position::CENTER;
					movement.to       = Position::LEFT_LASER;
					movement.progress = static_cast<double>(tub.position_um - leg_length_um) / leg;
				}
			} else if (tub.position_um > leg_length_um) {
				movement.from     = Position::LEFT_LASER;
				movement.to       = Position::CENTER;
				movement.progress = static_cast<double>((2 * leg_length_um) - tub.position_um) / leg;
			} else {
				movement.from     = Position::CENTER;
				movement.to       = Position::RIGHT_LASER;
				movement.progress = static_cast<double>(leg_length_um - tub.position_um) / leg;
			}
		} else if (speed > 0) {
			movement.from     = Position::CENTER;
			movement.to       = Position::ENTRY;
			movement.progress = static_cast<double>(tub.position_um) / leg;
		} else {
			movement.from     = Position::ENTRY;
			movement.to       = Position::CENTER;
			movement.progress = static_cast<double>(leg_length_um - tub.position_um) / leg;
		}

		movement.progress = clamp01(movement.progress);
		movement.moving   = true;
		return movement;
	}

	std::string to_string(Position position) {
		switch (position) {
		case Position::ENTRY:       return "entry";
		case Position::CENTER:      return "center";
		case Position::LEFT_LASER:  return "left_laser";
		case Position::RIGHT_LASER: return "right_laser";
		}
		return "center";
	}

	std::string to_string(TubRemovalReason reason) {
		switch (reason) {
		case TubRemovalReason::EXPLICIT:  return "explicit";
		case TubRemovalReason::SCHEDULED: return "scheduled";
		case TubRemovalReason::YEETED:    return "yeeted";
		}
		return "explicit";
	}

	std::optional<Position> position_from_string(const std::string& text) {
		if (text == "entry" || text == "rfid") {
			return Position::ENTRY;
		}
		if (text == "center") {
			return Position::CENTER;
		}
		if (text == "left_laser") {
			return Position::LEFT_LASER;
		}
		if (text == "right_laser") {
			return Position::RIGHT_LASER;
		}
		return {};
	}

} // namespace world
