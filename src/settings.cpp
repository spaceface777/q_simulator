#include "defs.h"
#include "lib/toml.h"
#include "settings.h"

namespace settings
{

    static ModuleSettings module_settings[MAX_MODULES];
    static toml::table table;

    void parse_settings()
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

    template <typename T>
    T get_setting(std::string_view setting)
    {
        if (!table.contains(setting))
        {
            throw std::invalid_argument("Invalid setting " + std::string(setting));
        }

        return table[setting].value<T>().value();
    }

    uint8_t get_modules_active()
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

    ModuleSettings get_module_settings(uint8_t id)
    {
        assert(id < MAX_MODULES);

        return module_settings[id];
    }

}

template int settings::get_setting<int>(std::string_view setting);
template bool settings::get_setting<bool>(std::string_view setting);
template std::string settings::get_setting<std::string>(std::string_view setting);