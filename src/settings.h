#ifndef SETTINGS_H
#define SETTINGS_H

#include <cassert>
#include <filesystem>

#include "defs.h"
#include "lib/toml.h"

namespace settings
{

    typedef struct
    {
        bool active;
        uint8_t left;
        uint8_t right;
        uint8_t bottom;
        uint16_t angle;
    } ModuleSettings;

    static ModuleSettings module_settings[MAX_MODULES];
    static toml::table table;

    /**
     * Parses the settings file.
     */
    inline void parse_settings()
    {
        try
        {
            table = toml::parse_file("settings.toml");
        }
        catch (const toml::parse_error &err)
        {
            throw err;
        }

        for (uint8_t id = 0; id < MAX_MODULES; id++)
        {
            toml::node *module_node = table.get("module");
            if (!module_node || !module_node->is_table())
            {
                throw std::invalid_argument("Invalid setting 'module' in settings.yml");
            }

            toml::table module_table = *module_node->as_table();
            toml::node *node = module_table.get(std::to_string(id + 1));
            if (!node || !node->is_table())
            {
                break;
            }

            toml::table table = *node->as_table();

            module_settings[id] = {
                .active = static_cast<bool>(table["active"].value_or(false)),
                .left = static_cast<uint8_t>(table["left"].value_or(0)),
                .right = static_cast<uint8_t>(table["right"].value_or(0)),
                .bottom = static_cast<uint8_t>(table["bottom"].value_or(0)),
                .angle = static_cast<uint16_t>(table["angle"].value_or(0)),
            };

            if (module_settings[id].angle >= 360)
            {
                throw std::invalid_argument("Invalid angle '" + std::to_string(module_settings[id].angle) + "' for module " + std::to_string(id));
            }
        }
    }

    /**
     * Gets the value of a specific setting.
     * Only returns generic types that are also in TOML.
     * @param name The name of the setting.
     * @returns The value of the setting.
     * @throws if the setting is not found.
     */
    template <typename T>
    inline T get_setting(std::string_view setting)
    {
        if (!table.contains(setting))
        {
            throw std::invalid_argument("Invalid setting " + std::string(setting));
        }

        return table[setting].value<T>().value();
    }

    /**
     * @return The amount of active modules.
     */
    inline uint8_t get_modules_active()
    {
        uint8_t count = 0;
        for (uint8_t id = 0; id < MAX_MODULES; id++)
        {
            if (module_settings[id].active)
            {
                count++;
            }
        }

        return count;
    }

    /**
     * Gets the settings of a specific module.
     * @param name The id of the module.
     * @returns The values of the module.
     * @throws if the module is not found.
     */
    inline ModuleSettings get_module_settings(uint8_t id)
    {
        assert(id < MAX_MODULES);

        return module_settings[id];
    }

}

#endif