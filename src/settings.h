#ifndef SETTINGS_H
#define SETTINGS_H

#include <cassert>
#include <filesystem>

#include "defs.h"
#include "lib/toml.h"

namespace settings
{
    namespace fs = std::filesystem;

    struct ModuleSettings
    {
        bool active;
        uint8_t left;
        uint8_t middle;
        uint8_t right;
        uint16_t angle;
        uint8_t x;
        uint8_t y;
    };

    /**
     * Parses the settings file.
     * @param file The settings file.
     */
    void parse_settings(fs::path file);

    /**
     * Gets the value of a specific setting.
     * Only returns generic types that are also in TOML.
     * @param name The name of the setting.
     * @returns The value of the setting.
     * @throws if the setting is not found.
     */
    template <typename T>
    T get_setting(std::string_view setting);

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
    uint8_t get_modules_active();

    /**
     * Gets the settings of a specific module.
     * @param name The id of the module.
     * @returns The values of the module.
     * @throws if the module is not found.
     */
    ModuleSettings get_module_settings(uint8_t id);

}

#endif