#include "defs.h"
#include "settings.h"
#include "util.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <toml++/toml.hpp>

namespace settings
{

    static ModuleSettings module_settings[MAX_MODULES];
    static toml::table settings_table;
    static toml::table layout_table;

    static void reset_state()
    {
        for (auto &module_setting : module_settings)
        {
            module_setting = ModuleSettings{};
        }

        settings_table = toml::table{};
        layout_table = toml::table{};
    }

    template <typename T, typename Predicate>
    static void parse_setting(std::string_view setting, T default_value, Predicate pred)
    {
        toml::node *node = settings_table.get(setting);
        util::require(node, "Missing setting '" + std::string(setting) + "' in settings.toml");

        T value = static_cast<T>(node->value_or(default_value));
        util::require(pred(value), "Invalid value for '" + std::string(setting) + "' in settings.toml");

        settings::set_setting(setting, value);
    }

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

                const auto &dest = module_settings[target_id];

                ModuleLocation targeted_port;
                if (dest.left == id)
                {
                    targeted_port = ModuleLocation::LEFT;
                }
                else if (dest.right == id)
                {
                    targeted_port = ModuleLocation::RIGHT;
                }
                else if (dest.middle == id)
                {
                    targeted_port = ModuleLocation::MIDDLE;
                }
                else
                {
                    util::require(false,
                                  "Module " + std::to_string(id) + " connects to module " + std::to_string(target_id) +
                                      " via " + std::string(port_name) + ", but module " + std::to_string(target_id) +
                                      " does not connect back to module " + std::to_string(id));
                    return;
                }

                auto location_key = std::make_pair(target_id, targeted_port);
                util::require(claimed_ports.find(location_key) == claimed_ports.end(),
                              "Multiple modules attempt to connect to module " + std::to_string(target_id) +
                                  "'s port");

                claimed_ports.insert(location_key);
            };

            if (src.left)
                check_connection(src.left->raw(), "left");
            if (src.right)
                check_connection(src.right->raw(), "right");
            if (src.middle)
                check_connection(src.middle->raw(), "middle");
        }
    }

    static std::optional<module::ModuleId> parse_neighbor(int64_t id)
    {
        if (id == 0)
            return {};
        return module::ModuleId(id);
    }

    void parse_settings(fs::path settings, fs::path layout)
    {
        util::require(fs::exists(settings), "No settings.toml found in same folder as simulation executable");
        util::require(fs::exists(layout), "No layout.toml found in same folder as simulation executable");

        reset_state();

        try
        {
            settings_table = toml::parse_file(settings.string());
            layout_table = toml::parse_file(layout.string());
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
            toml::node *module_node = layout_table.get("module");
            if (!module_node || !module_node->is_table())
            {
                break;
            }

            toml::table module_table = *module_node->as_table();
            toml::node *node = module_table.get(std::to_string(id));
            if (!node || !node->is_table())
            {
                continue;
            }

            toml::table table = *node->as_table();

            {
                auto opt_active = table["active"].value_exact<bool>();
                util::require(opt_active.has_value(), "Missing or invalid 'active' attribute in module " + std::to_string(id));
                module_settings[id].active = opt_active.value();
            }
            {
                auto opt_angle = table["angle"].value_exact<int64_t>();
                util::require_range(opt_angle.has_value() && opt_angle.value() >= 0 && opt_angle.value() <= 270,
                                    "Missing or invalid 'angle' attribute in module " + std::to_string(id));
                module_settings[id].angle = ModuleAngle(static_cast<uint16_t>(opt_angle.value()));
            }
            {
                toml::v3::array *pos = table["pos"].as_array();
                util::require(pos != nullptr && pos->is_array(), "Attribute 'pos' not found for module " + std::to_string(id));
                util::require(pos->size() == 2, "Invalid value for attribute 'pos' of module " + std::to_string(id));

                module_settings[id].x = static_cast<uint8_t>(pos->get(0)->value_exact<int64_t>().value());
                module_settings[id].y = static_cast<uint8_t>(pos->get(1)->value_exact<int64_t>().value());

                util::require(module_settings[id].x <= 10, "Invalid value for attribute 'pos' of module " + std::to_string(id));
                util::require(module_settings[id].y <= 10, "Invalid value for attribute 'pos' of module " + std::to_string(id));
            }
            {
                toml::v3::array *neighbors = table["neighbors"].as_array();
                util::require(neighbors != nullptr && neighbors->is_array(), "Attribute 'neighbors' not found for module " + std::to_string(id));
                util::require(neighbors->size() == 3, "Invalid value for attribute 'neighbors' of module " + std::to_string(id));

                module_settings[id].left = parse_neighbor(neighbors->get(0)->value_exact<int64_t>().value());
                module_settings[id].middle = parse_neighbor(neighbors->get(1)->value_exact<int64_t>().value());
                module_settings[id].right = parse_neighbor(neighbors->get(2)->value_exact<int64_t>().value());
            }
        }

        validate_module_connections();

        set_setting("debug", false);
    }

    template <typename T>
    T get_setting(std::string_view setting)
    {
        util::require(settings_table.contains(setting), "Invalid setting '" + std::string(setting) + "'");
        return settings_table[setting].value<T>().value();
    }

    template <typename T>
    void set_setting(std::string_view setting, T value)
    {
        settings_table.insert_or_assign(std::string(setting), value);
    }

    std::size_t get_modules_active()
    {
        std::size_t count = 0;
        for (std::size_t id = 1; id < MAX_MODULES; id++)
        {
            if (module_settings[id].active)
            {
                count++;
            }
        }

        return count;
    }

    ModuleSettings get_module_settings(module::ModuleId id)
    {
        return module_settings[id.raw()];
    }

}

template int settings::get_setting<int>(std::string_view setting);
template double settings::get_setting<double>(std::string_view setting);
template bool settings::get_setting<bool>(std::string_view setting);
template std::string settings::get_setting<std::string>(std::string_view setting);

template void settings::set_setting<double>(std::string_view setting, double value);
template void settings::set_setting<bool>(std::string_view setting, bool value);
