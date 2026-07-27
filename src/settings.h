#ifndef SETTINGS_H
#define SETTINGS_H

#include <cassert>
#include <filesystem>
#include <optional>

#include "defs.h"
#include "module/types.h"

namespace settings
{
    namespace fs = std::filesystem;

    enum class ModuleAngle : uint16_t
    {
        NORMAL = 0,        // T shape
        ROTATED_90 = 90,   // ┤ shape
        UPSIDE_DOWN = 180, // ┴ shape
        ROTATED_270 = 270, // ├ shape
    };

    struct ModuleSettings
    {
        bool active;
        std::optional<module::ModuleId> left;
        std::optional<module::ModuleId> middle;
        std::optional<module::ModuleId> right;
        ModuleAngle angle;
        uint8_t x;
        uint8_t y;
    };

    /**
     * Parses the settings file.
     * @param file The settings file.
     */
    void parse_settings(fs::path settings, fs::path layout);

    /**
     * Gets the value of a specific setting.
     * Only returns generic types that are also in TOML.
     * @param name The name of the setting.
     * @returns The value of the setting.
     * @throws if the setting is not found.
     */
    template <typename T>
    [[nodiscard]] T get_setting(std::string_view setting);

    /**
     * Sets the value of a setting.
     * @param setting The name of the setting,.
     * @param value The value to set the setting to.
     * @throws If the setting is not found.
     */
    template <typename T>
    void set_setting(std::string_view setting, T value);

    /**
     * @return The amount of active modules.
     */
    [[nodiscard]] std::size_t get_modules_active();

    /**
     * Gets the settings of a specific module.
     * @param name The id of the module.
     * @returns The values of the module.
     * @throws if the module is not found.
     */
    [[nodiscard]] ModuleSettings get_module_settings(module::ModuleId id);

}

#endif