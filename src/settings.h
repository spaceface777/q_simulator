#ifndef SETTINGS_H
#define SETTINGS_H

#include <filesystem>

#include "lib/toml.h"

namespace settings
{
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

}

#endif