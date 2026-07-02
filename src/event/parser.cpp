#include <list>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <string>

#include "../util.h"
#include "../defs.h"
#include "parser.h"
#include "../lib/toml.h"

/**
 * Parses the events in the events.toml file.
 * @param result Vector to populate with parsed events.
 */
void parser::parse_events(std::list<events::Event> &result)
{

    toml::table table;
    try
    {
        table = toml::parse_file("events.toml");
    }
    catch (const toml::parse_error &err)
    {
        throw err;
    }

    for (auto &&[key, event] : table)
    {
        std::string group = std::string(key.str());

        if (!event.is_table())
            continue;

        auto &tbl = *event.as_table();

        if (group == "tub")
        {
            for (auto &&[event_key, event_value] : tbl)
            {
                if (!event_value.is_table())
                    continue;

                auto &event_tbl = *event_value.as_table();

                int time = event_tbl["time"].value_or(-1);
                int id = event_tbl["id"].value_or(-1);
                bool safe = event_tbl["safe"].value_or(false);

                // Parse origin
                events::TubData origin{events::LocationType::MODULE, -1};
                if (event_tbl.contains("origin") && event_tbl["origin"].is_array())
                {
                    auto &origin_arr = *event_tbl["origin"].as_array();
                    if (origin_arr.size() >= 2)
                    {
                        std::string location_type = origin_arr[0].value_or("");
                        int location_id = origin_arr[1].value_or(-1);

                        if (location_type == "module")
                        {
                            origin.type = events::LocationType::MODULE;
                        }
                        else if (location_type == "plane")
                        {
                            origin.type = events::LocationType::PLANE;
                        }
                        else
                        {
                            throw std::invalid_argument("Invalid origin location type: " + location_type);
                        }
                        origin.id = location_id;
                    }
                }

                // Parse destination
                events::TubData destination{events::LocationType::MODULE, -1};
                if (event_tbl.contains("destination") && event_tbl["destination"].is_array())
                {
                    auto &dest_arr = *event_tbl["destination"].as_array();
                    if (dest_arr.size() == 2)
                    {
                        std::string location_type = dest_arr[0].value_or("");
                        int location_id = dest_arr[1].value_or(-1);

                        if (location_type == "module")
                        {
                            destination.type = events::LocationType::MODULE;
                        }
                        else if (location_type == "plane")
                        {
                            destination.type = events::LocationType::PLANE;
                        }
                        else
                        {
                            throw std::invalid_argument("Invalid destination location type: " + location_type);
                        }
                        destination.id = location_id;
                    }
                }

                if (time == -1 || id == -1 || origin.id == -1 || destination.id == -1)
                {
                    throw std::invalid_argument("Tub event missing required fields");
                }

                result.push_back(events::TubEvent(time, id, safe, origin, destination));
            }
        }
        else if (group == "plane")
        {
            for (auto &&[event_key, event_value] : tbl)
            {
                if (!event_value.is_table())
                    continue;

                auto &event_tbl = *event_value.as_table();

                int time = event_tbl["time"].value_or(-1);
                int id = event_tbl["id"].value_or(-1);
                int module = event_tbl["module"].value_or(-1);
                int leaves_in = event_tbl["leaves_in"].value_or(-1);

                if (time == -1 || id == -1 || module == -1 || leaves_in == -1)
                {
                    throw std::invalid_argument("Plane event missing required fields");
                }

                result.push_back(events::PlaneEvent(time, id, module, leaves_in));
            }
        }
    }
}
