#ifndef SETTINGS_H
#define SETTINGS_H

#include <cassert>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "defs.h"
#include "module/types.h"

namespace settings {
	namespace fs = std::filesystem;

	enum class ModuleAngle : uint16_t {
		NORMAL      = 0,   // T shape
		ROTATED_90  = 90,  // ┤ shape
		UPSIDE_DOWN = 180, // ┴ shape
		ROTATED_270 = 270, // ├ shape
	};

	struct ModuleSettings {
		// Configured records whether this ID has an entry in layout.toml. An inactive configured
		// module is known to the network even though no application runtime is started for it.
		bool                            configured = false;
		bool                            active     = false;
		std::optional<module::ModuleId> left;
		std::optional<module::ModuleId> middle;
		std::optional<module::ModuleId> right;
		ModuleAngle                     angle;
		uint8_t                         x;
		uint8_t                         y;

		// The role advertised in the network map, without the leading 't'. The tower looks for "gate".
		std::string type = "default";

		// The application to run for this module. Empty means the global application_path.
		std::string binary;
	};

	void parse_settings(fs::path settings, fs::path layout,
						std::optional<fs::path> application_path_override = std::nullopt);

	// Only returns generic types that are also in TOML. Throws if the setting is not found.
	template <typename T> [[nodiscard]] T get_setting(std::string_view setting);

	// Throws if the setting is not found.
	template <typename T> void set_setting(std::string_view setting, T value);

	[[nodiscard]] std::size_t get_modules_active();

	// Ascending. Includes the tower when configured.
	[[nodiscard]] std::vector<module::ModuleId> get_active_modules();

	// Ascending. Includes inactive module entries from layout.toml.
	[[nodiscard]] std::vector<module::ModuleId> get_configured_modules();

	// The module's own binary override when set, otherwise the global application_path.
	[[nodiscard]] std::string get_module_binary(module::ModuleId id);

	// Throws if the module is not found.
	[[nodiscard]] ModuleSettings get_module_settings(module::ModuleId id);

#ifdef TESTING

	void set_module_settings(module::ModuleId id, ModuleSettings settings);

	// Installs everything parse_settings would have guaranteed, without a settings.toml.
	void install_test_defaults();

#endif

} // namespace settings

#endif
