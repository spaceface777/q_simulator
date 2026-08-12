#include "settings.h"

#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

#include "defs.h"
#include "util.h"
#include "world/calibration.h"

namespace settings {

	static ModuleSettings module_settings[MODULE_TABLE_SIZE];
	static toml::table    settings_table;
	static toml::table    layout_table;

	static void reset_state() {
		for (auto& module_setting : module_settings) {
			module_setting = ModuleSettings{};
		}

		settings_table = toml::table{};
		layout_table   = toml::table{};
	}

	template <typename T, typename Predicate>
	static void parse_setting(std::string_view setting, T default_value, Predicate pred) {
		toml::node* node = settings_table.get(setting);
		util::require(node, "Missing setting '" + std::string(setting) + "' in settings.toml");

		T value = static_cast<T>(node->value_or(default_value));
		util::require(pred(value), "Invalid value for '" + std::string(setting) + "' in settings.toml");

		settings::set_setting(setting, value);
	}

	// A setting with a usable default, so a new knob does not invalidate an older settings.toml.
	template <typename T, typename Predicate>
	static void parse_optional_setting(std::string_view setting, T fallback, Predicate pred) {
		const toml::node* node = settings_table.get(setting);
		if (node == nullptr) {
			settings::set_setting(setting, fallback);
			return;
		}

		T value = static_cast<T>(node->value_or(fallback));
		util::require(pred(value), "Invalid value for '" + std::string(setting) + "' in settings.toml");
		settings::set_setting(setting, value);
	}

	// Every id a module may have: the routing range followed by the tower.
	static std::vector<uint8_t> all_module_ids() {
		std::vector<uint8_t> ids;
		ids.reserve(MAX_MODULES);
		for (uint8_t id = 1; id < MAX_MODULES; id++) {
			ids.push_back(id);
		}
		ids.push_back(TOWER_MODULE_ID);
		return ids;
	}

	static void validate_module_connections() {
		std::set<std::pair<uint8_t, ModuleLocation>> claimed_ports;

		for (uint8_t id = 1; id < MAX_MODULES; id++) {
			const auto& src = module_settings[id];

			auto check_connection = [&](uint8_t target_id, std::string_view port_name) {
				if (target_id == 0) return;

				const auto& dest = module_settings[target_id];

				ModuleLocation targeted_port;
				if (dest.left == id) {
					targeted_port = ModuleLocation::LEFT;
				} else if (dest.right == id) {
					targeted_port = ModuleLocation::RIGHT;
				} else if (dest.middle == id) {
					targeted_port = ModuleLocation::MIDDLE;
				} else {
					util::require(false, "Module " + std::to_string(id) + " connects to module " +
											 std::to_string(target_id) + " via " + std::string(port_name) +
											 ", but module " + std::to_string(target_id) +
											 " does not connect back to module " + std::to_string(id));
					return;
				}

				auto location_key = std::make_pair(target_id, targeted_port);
				util::require(claimed_ports.find(location_key) == claimed_ports.end(),
							  "Multiple modules attempt to connect to module " + std::to_string(target_id) + "'s port");

				claimed_ports.insert(location_key);
			};

			if (src.left) check_connection(src.left->raw(), "left");
			if (src.right) check_connection(src.right->raw(), "right");
			if (src.middle) check_connection(src.middle->raw(), "middle");
		}
	}

	static std::optional<module::ModuleId> parse_neighbor(int64_t id) {
		if (id == 0) return {};
		return module::ModuleId{static_cast<uint8_t>(id)};
	}

	// Reads one [module.N] table. Geometry is optional for the tower, required for routing modules.
	static void parse_module(uint8_t id, const toml::table& table) {
		const bool is_tower = id == TOWER_MODULE_ID;

		{
			auto opt_active = table["active"].value_exact<bool>();
			util::require(opt_active.has_value(),
						  "Missing or invalid 'active' attribute in module " + std::to_string(id));
			module_settings[id].active = opt_active.value();
		}
		{
			auto opt_type = table["type"].value<std::string>();
			if (opt_type.has_value()) {
				util::require(!opt_type.value().empty(), "Empty 'type' attribute in module " + std::to_string(id));
				module_settings[id].type = opt_type.value();
			}
		}
		{
			auto opt_binary = table["binary"].value<std::string>();
			if (opt_binary.has_value()) {
				util::require(fs::exists(opt_binary.value()),
							  "No application found at 'binary' of module " + std::to_string(id));
				module_settings[id].binary = opt_binary.value();
			}
		}
		{
			auto opt_angle = table["angle"].value_exact<int64_t>();
			if (opt_angle.has_value() || !is_tower) {
				util::require_range(opt_angle.has_value() && opt_angle.value() >= 0 && opt_angle.value() <= 270,
									"Missing or invalid 'angle' attribute in module " + std::to_string(id));
				module_settings[id].angle = ModuleAngle(static_cast<uint16_t>(opt_angle.value()));
			}
		}
		{
			const toml::array* pos = table["pos"].as_array();
			if (pos != nullptr || !is_tower) {
				util::require(pos != nullptr && pos->is_array(),
							  "Attribute 'pos' not found for module " + std::to_string(id));
				util::require(pos->size() == 2, "Invalid value for attribute 'pos' of module " + std::to_string(id));

				module_settings[id].x = static_cast<uint8_t>(pos->get(0)->value_exact<int64_t>().value());
				module_settings[id].y = static_cast<uint8_t>(pos->get(1)->value_exact<int64_t>().value());

				util::require(module_settings[id].x <= 10,
							  "Invalid value for attribute 'pos' of module " + std::to_string(id));
				util::require(module_settings[id].y <= 10,
							  "Invalid value for attribute 'pos' of module " + std::to_string(id));
			}
		}
		{
			const toml::array* neighbors = table["neighbors"].as_array();
			if (neighbors != nullptr || !is_tower) {
				util::require(neighbors != nullptr && neighbors->is_array(),
							  "Attribute 'neighbors' not found for module " + std::to_string(id));
				util::require(neighbors->size() == 3,
							  "Invalid value for attribute 'neighbors' of module " + std::to_string(id));

				module_settings[id].left   = parse_neighbor(neighbors->get(0)->value_exact<int64_t>().value());
				module_settings[id].middle = parse_neighbor(neighbors->get(1)->value_exact<int64_t>().value());
				module_settings[id].right  = parse_neighbor(neighbors->get(2)->value_exact<int64_t>().value());
			}
		}
	}

	// One list, shared with the test build's install_test_defaults() so the two cannot drift.
	static void parse_optional_settings() {
		const world::CalibrationConfig calibration_defaults;
		const auto                     valid_position_mean = [](double value) {
			return std::isfinite(value) && std::abs(value) <= world::MAX_ABSOLUTE_HARDWARE_POSITION_FRACTION;
		};
		const auto valid_position_sd = [](double value) {
			return std::isfinite(value) && value >= 0.0 && value <= world::MAX_ABSOLUTE_HARDWARE_POSITION_FRACTION;
		};
		parse_optional_setting("network_packet_loss_ppm", 1700,
							   [](int value) { return value >= 0 && value <= 1000000; });
		parse_optional_setting("network_very_late_ppm", 15, [](int value) { return value >= 0 && value <= 1000000; });
		parse_optional_setting("network_very_late_delay_ms", 1100, [](int value) { return value >= 0; });
		// -1 means: use the profile's delay distribution instead of a fixed delay.
		parse_optional_setting("network_fixed_delay_ms", -1, [](int value) { return value >= -1 && value <= 10000; });
		// Wi-Fi deployment does not start every module at simulation tick zero. Each module samples
		// an independent, deterministic non-negative delay. A zero standard deviation is useful for
		// protocol unit scenarios without weakening the default physical model.
		parse_optional_setting("module_start_offset_mean_ms", 300.0, [](double value) { return value >= 0.0; });
		parse_optional_setting("module_start_offset_sd_ms", 100.0, [](double value) { return value >= 0.0; });
		// Independent oscillator error. 100 ppm is a conservative default for inexpensive clocks;
		// local uptime is skewed while world, network, and property time remain authoritative.
		parse_optional_setting("module_clock_drift_sd_ppm", 100.0, [](double value) { return value >= 0.0; });
		// A module-specific probability is sampled once; each global tick then independently decides
		// whether that module's local clock misses the tick. 1000 ppm is 0.1 percent.
		parse_optional_setting("module_tick_skip_mean_ppm", 1000.0,
							   [](double value) { return value >= 0.0 && value <= 1000000.0; });
		parse_optional_setting("module_tick_skip_sd_ppm", 100.0,
							   [](double value) { return value >= 0.0 && value <= 1000000.0; });
		// The extra hop a log or panic report takes: Core 0 to the hub Pi, which then forwards it
		// on, rather than Core 0 to its destination directly.
		parse_optional_setting("network_log_extra_delay_ms", 2, [](int value) { return value >= 0 && value <= 10000; });
		parse_optional_setting<std::string>("trace_path", "trace.ndjson", [](const std::string&) { return true; });

		// The calibration tunables. These are the figures docs/physical_calibration_2026-08-06.md
		// marks as assumptions rather than measurements - a population prior and the provisional
		// servo timing - so they are the ones worth being able to change without a rebuild. The
		// measured per-module distributions are not settings: changing those means the bench found
		// something different, which is a change to the model and not to a run.
		parse_optional_setting("weak_motor_rate", calibration_defaults.weak_motor_rate,
							   [](double value) { return value >= 0.0 && value <= 1.0; });
		parse_optional_setting("small_to_big_belt_speed_ratio", calibration_defaults.small_to_big_belt_speed_ratio,
							   [](double value) { return value > 0.0 && value <= 10.0; });
		parse_optional_setting("servo_full_sweep_ms", calibration_defaults.servo_full_sweep_ms,
							   [](double value) { return value > 0.0; });
		parse_optional_setting("servo_module_offset_sd_ms", calibration_defaults.servo_module_offset_sd_ms,
							   [](double value) { return value >= 0.0; });
		parse_optional_setting("servo_movement_jitter_sd_ms", calibration_defaults.servo_movement_jitter_sd_ms,
							   [](double value) { return value >= 0.0; });
		parse_optional_setting("left_laser_position_mean", calibration_defaults.left_laser_position_mean,
							   valid_position_mean);
		parse_optional_setting("left_laser_position_sd", calibration_defaults.left_laser_position_sd,
							   valid_position_sd);
		parse_optional_setting("right_laser_position_mean", calibration_defaults.right_laser_position_mean,
							   valid_position_mean);
		parse_optional_setting("right_laser_position_sd", calibration_defaults.right_laser_position_sd,
							   valid_position_sd);
		parse_optional_setting("rfid_position_mean", calibration_defaults.rfid_position_mean, valid_position_mean);
		parse_optional_setting("rfid_position_sd", calibration_defaults.rfid_position_sd, valid_position_sd);
	}

	void parse_settings(fs::path settings, fs::path layout) {
		util::require(fs::exists(settings), "No settings.toml found in same folder as simulation executable");
		util::require(fs::exists(layout), "No layout.toml found in same folder as simulation executable");

		reset_state();

		try {
			settings_table = toml::parse_file(settings.string());
			layout_table   = toml::parse_file(layout.string());
		} catch (const toml::parse_error& err) {
			throw err;
		}

		parse_setting("application_path", "", [](std::string value) {
			util::require(std::filesystem::exists(value), "No application found at application_path in settings.toml");
			return true;
		});
		parse_setting("simulation_speed", -1.0, [](double value) { return value >= 0; });
		parse_setting("seed", -1, [](int value) { return value >= 0; });
		parse_setting("scale", -1.0, [](double value) { return value >= 0.25 && value <= 5; });
		parse_optional_settings();

		toml::node* module_node = layout_table.get("module");
		if (module_node != nullptr && module_node->is_table()) {
			const toml::table module_table = *module_node->as_table();

			for (const uint8_t id : all_module_ids()) {
				const toml::node* node = module_table.get(std::to_string(id));
				if (!node || !node->is_table()) {
					continue;
				}

				parse_module(id, *node->as_table());
			}
		}

		validate_module_connections();

		set_setting("debug", false);
	}

	template <typename T> T get_setting(std::string_view setting) {
		util::require(settings_table.contains(setting), "Invalid setting '" + std::string(setting) + "'");
		return settings_table[setting].value<T>().value();
	}

	template <typename T> void set_setting(std::string_view setting, T value) {
		settings_table.insert_or_assign(std::string(setting), value);
	}

	std::size_t get_modules_active() {
		return get_active_modules().size();
	}

	std::vector<module::ModuleId> get_active_modules() {
		std::vector<module::ModuleId> active;
		for (const uint8_t id : all_module_ids()) {
			if (module_settings[id].active) {
				active.push_back(module::ModuleId{id});
			}
		}
		return active;
	}

	std::string get_module_binary(module::ModuleId id) {
		const std::string& override_path = module_settings[id.raw()].binary;
		if (!override_path.empty()) {
			return override_path;
		}
		return get_setting<std::string>("application_path");
	}

	ModuleSettings get_module_settings(module::ModuleId id) {
		return module_settings[id.raw()];
	}

#ifdef TESTING

	void set_module_settings(module::ModuleId id, ModuleSettings settings) {
		module_settings[id.raw()] = settings;
	}

	void install_test_defaults() {
		settings_table = toml::table{};
		set_setting<std::string>("application_path", "");
		set_setting("simulation_speed", 1.0);
		set_setting("seed", 1);
		set_setting("scale", 1.0);
		set_setting("debug", false);
		parse_optional_settings();
	}

#endif

} // namespace settings

template int         settings::get_setting<int>(std::string_view setting);
template double      settings::get_setting<double>(std::string_view setting);
template bool        settings::get_setting<bool>(std::string_view setting);
template std::string settings::get_setting<std::string>(std::string_view setting);

template void settings::set_setting<double>(std::string_view setting, double value);
template void settings::set_setting<bool>(std::string_view setting, bool value);
template void settings::set_setting<int>(std::string_view setting, int value);
template void settings::set_setting<std::string>(std::string_view setting, std::string value);
