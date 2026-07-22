#include "defs.h"
#include "lib/toml.h"
#include "settings.h"
#include "util.h"
#include <functional>
#include <map>
#include <set>
#include <string>

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

    enum class OutputType
    {
        LEFT,
        RIGHT,
        BOTTOM
    };

    static void validate_module_connections()
    {
        std::set<std::pair<uint8_t, ModuleLocation>> claimed_ports;

        for (uint8_t id = 1; id < MAX_MODULES; id++)
        {
            const auto &src = module_settings[id];

            auto check_connection = [&](uint8_t target_id, std::string_view port_name)
            {
                if (target_id == 0)
                    return;

                util::require(target_id < MAX_MODULES,
                              "Module " + std::to_string(id) + " connects to invalid or inactive module " + std::to_string(target_id));

                const auto &dest = module_settings[target_id];

                bool connects_back = false;
                ModuleLocation targeted_port;

                if (dest.left == id)
                {
                    connects_back = true;
                    targeted_port = ModuleLocation::LEFT;
                }
                else if (dest.right == id)
                {
                    connects_back = true;
                    targeted_port = ModuleLocation::RIGHT;
                }
                else if (dest.bottom == id)
                {
                    connects_back = true;
                    targeted_port = ModuleLocation::BOTTOM;
                }

                util::require(connects_back,
                              "Module " + std::to_string(id) + " connects to module " + std::to_string(target_id) +
                                  " via " + std::string(port_name) + ", but module " + std::to_string(target_id) +
                                  " does not connect back to module " + std::to_string(id));

                auto location_key = std::make_pair(target_id, targeted_port);
                util::require(claimed_ports.find(location_key) == claimed_ports.end(),
                              "Multiple modules attempt to connect to module " + std::to_string(target_id) +
                                  "'s port");

                claimed_ports.insert(location_key);
            };

            check_connection(src.left, "left");
            check_connection(src.right, "right");
            check_connection(src.bottom, "bottom");
        }
    }

    void parse_settings(fs::path file)
    {
        try
        {
            table = toml::parse_file(file.string());
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
                continue;
            }

            toml::table table = *node->as_table();

            auto opt_active = table["active"].value_exact<bool>();
            util::require(opt_active.has_value(), "Missing or invalid 'active' attribute in module " + std::to_string(id));
            module_settings[id].active = opt_active.value();

            auto opt_left = table["left"].value_exact<int64_t>();
            util::require(opt_left.has_value() && opt_left.value() >= 0 && opt_left.value() < MAX_MODULES,
                          "Missing or invalid 'left' attribute in module " + std::to_string(id));
            module_settings[id].left = static_cast<uint8_t>(opt_left.value());

            auto opt_right = table["right"].value_exact<int64_t>();
            util::require(opt_right.has_value() && opt_right.value() >= 0 && opt_right.value() < MAX_MODULES,
                          "Missing or invalid 'right' attribute in module " + std::to_string(id));
            module_settings[id].right = static_cast<uint8_t>(opt_right.value());

            auto opt_bottom = table["bottom"].value_exact<int64_t>();
            util::require(opt_bottom.has_value() && opt_right.value() >= 0 && opt_right.value() < MAX_MODULES,
                          "Missing or invalid 'bottom' attribute in module " + std::to_string(id));
            module_settings[id].bottom = static_cast<uint8_t>(opt_bottom.value());

            auto opt_angle = table["angle"].value_exact<int64_t>();
            util::require(opt_angle.has_value() && opt_angle.value() >= 0 && opt_angle.value() < 360,
                          "Missing or invalid 'angle' attribute in module " + std::to_string(id));
            module_settings[id].angle = static_cast<uint16_t>(opt_angle.value());

            toml::v3::array *pos = table["pos"].as_array();
            util::require(pos != nullptr && pos->is_array(), "Attribute 'pos' not found for module " + std::to_string(id));
            util::require(pos->size() == 2, "Invalid value for attribute 'pos' of module " + std::to_string(id));

            module_settings[id].x = static_cast<uint8_t>(pos->get(0)->value_exact<int64_t>().value());
            module_settings[id].y = static_cast<uint8_t>(pos->get(1)->value_exact<int64_t>().value());
        }

        validate_module_connections();

        set_setting("debug", false);
    }

    template <typename T>
    T get_setting(std::string_view setting)
    {
        util::require(table.contains(setting), "Invalid setting '" + std::string(setting) + "'");
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
