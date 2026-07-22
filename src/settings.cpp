#include "defs.h"
#include "lib/toml.h"
#include "settings.h"
#include "util.h"
#include <functional>

namespace settings
{

    static ModuleSettings module_settings[MAX_MODULES];
    static toml::table table;

    template <typename T, typename Predicate>
    static void parse_setting(std::string_view setting, T default_value, Predicate pred)
    {
        toml::node *node = table.get(setting);
        util::require(node, "Missing setting '" + std::string(setting) + "' in settings.toml");

        T value = static_cast<T>(node->value_or(default_value));
        util::require(pred(value), "Invalid value for '" + std::string(setting) + "' in settings.toml");

        settings::set_setting(setting, value);
    }

    void parse_settings(std::string_view file)
    {
        try
        {
            table = toml::parse_file(file);
        }
        catch (const toml::parse_error &err)
        {
            throw err;
        }

        parse_setting("router_path", "", [](std::string value)
                      { util::require(std::filesystem::exists(value), "No zephyr.exe found at router_path in settings.toml");
                        return true; });
        parse_setting("simulation_speed", -1.0, [](double value)
                      { return value >= 0; });
        parse_setting("scale", -1.0, [](double value)
                      { return value >= 0.25 && value <= 5; });

        for (uint8_t id = 1; id < MAX_MODULES; id++)
        {
            toml::node *module_node = table.get("module");
            util::require(module_node && module_node->is_table(), "Invalid setting 'module' in settings.toml");

            toml::table module_table = *module_node->as_table();
            toml::node *node = module_table.get(std::to_string(id));
            if (!node || !node->is_table())
            {
                break;
            }

            toml::table table = *node->as_table();

            module_settings[id].active = static_cast<bool>(table["active"].value_or(false));
            module_settings[id].left = static_cast<uint8_t>(table["left"].value_or(0));
            module_settings[id].right = static_cast<uint8_t>(table["right"].value_or(0));
            module_settings[id].bottom = static_cast<uint8_t>(table["bottom"].value_or(0));
            module_settings[id].angle = static_cast<uint16_t>(table["angle"].value_or(0));

            util::require(module_settings[id].angle < 360, "Invalid angle '" + std::to_string(module_settings[id].angle) + "' for module " + std::to_string(id));
        }

        settings::set_setting("debug", false);
    }

    template <typename T>
    T get_setting(std::string_view setting)
    {
        util::require(table.contains(setting), "Invalid setting " + std::string(setting));
        return table[setting].value<T>().value();
    }

    template <typename T>
    void set_setting(std::string_view setting, T value)
    {
        table.insert_or_assign(std::string(setting), value);
    }

    uint8_t get_modules_active()
    {
        uint8_t count = 0;
        for (uint8_t id = 1; id < MAX_MODULES; id++)
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
        util::require(id >= 1 && id < MAX_MODULES, "Invalid module id " + std::to_string(id));
        return module_settings[id];
    }

}

template int settings::get_setting<int>(std::string_view setting);
template double settings::get_setting<double>(std::string_view setting);
template bool settings::get_setting<bool>(std::string_view setting);
template std::string settings::get_setting<std::string>(std::string_view setting);

template void settings::set_setting<double>(std::string_view setting, double value);
template void settings::set_setting<bool>(std::string_view setting, bool value);
