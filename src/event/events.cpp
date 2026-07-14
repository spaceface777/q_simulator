#include "events.h"
#include "../message/message.h"
#include "../module/module.h"
#include "../tub/tub.h"
#include "../lib/toml.h"
#include "../defs.h"

#include <memory>
#include <iostream>

namespace events
{

    void fire_events(virtual_clock::VirtualClock &vclock, std::list<std::unique_ptr<Event>> &events)
    {
        int64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(vclock.get_virtual_time()).count();

        events.remove_if([seconds](const auto &event)
                         {
                             if (seconds >= event->time())
                             {
                                 event->fire();
                                 std::cout << "Fired at vtime " << std::to_string(seconds) << std::endl;
                                 return true;
                             }
                             return false; });
    }

    void PlaneEvent::fire() const
    {
        auto message = message::PlaneArrival(id_, module_, leaves_in_);

        for (const auto &module : module::get_modules())
        {
            if (!module)
            {
                continue;
            }

            module->send(message);
        }

        std::cout << "Fired plane event at " << std::to_string(time_) << std::endl;
    }

    void TubEvent::fire() const
    {
        const auto message = message::TubArrival(id_, origin_.id, destination_.id, safe_, needs_security_);
        const auto module = module::get_module(origin_.id);

        util::require(module.has_value(), "No module with id " + std::to_string(origin_.id));
        (*module)->send(message);

        auto tub_ptr = std::make_unique<tub::Tub>(id_, origin_.id, destination_.id, ModuleLocation::SMALL_BELT, rfid_);
        tub_ptr->run();
        tub::add_tub(std::move(tub_ptr));

        std::cout << "Fired tub event at " << std::to_string(time_) << std::endl;
    }

    static std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> parse_rfid(toml::v3::array *rfid_arr)
    {
        if (!rfid_arr)
        {
            throw std::invalid_argument("Tub event missing rfid array");
        }
        if (rfid_arr->size() > RFID_CONTENT_BLOCK_COUNT)
        {
            throw std::invalid_argument("rfid contains " + std::to_string(rfid_arr->size()) + " blocks, which is more than the 36 allowed");
        }

        std::vector<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>> parsed_rfid;
        parsed_rfid.reserve(RFID_CONTENT_BLOCK_COUNT);
        for (auto &&node : *rfid_arr)
        {
            if (auto *inner_arr = node.as_array())
            {
                std::array<int8_t, RFID_BLOCK_SIZE_BYTES> row = {0, 0, 0, 0};
                if (inner_arr->size() != 4)
                {
                    throw std::invalid_argument("Invalid array in rfid data");
                }

                for (size_t i = 0; i < RFID_BLOCK_SIZE_BYTES; ++i)
                {
                    row[i] = inner_arr->get(i)->as_integer()->get();
                }

                parsed_rfid.push_back(row);
            }
            else
            {
                throw std::invalid_argument("Invalid array in rfid data");
            }
        }

        while (parsed_rfid.size() < RFID_CONTENT_BLOCK_COUNT)
        {
            parsed_rfid.push_back({0, 0, 0, 0});
        }

        std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid_fixed_array;
        std::copy(parsed_rfid.begin(), parsed_rfid.end(), rfid_fixed_array.begin());
        return rfid_fixed_array;
    }

    void parse_events(std::list<std::unique_ptr<Event>> &result)
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

                    double time = event_tbl["time"].value_or(-1.0);
                    int id = event_tbl["id"].value_or(-1);
                    bool safe = event_tbl["safe"].value_or(false);
                    bool needs_security = event_tbl["needs_security"].value_or(false);
                    toml::v3::array *rfid_arr = event_tbl["rfid"].as_array();

                    // Parse origin
                    events::TubData origin{LocationType::MODULE, -1};
                    if (event_tbl.contains("origin") && event_tbl["origin"].is_array())
                    {
                        auto &origin_arr = *event_tbl["origin"].as_array();
                        if (origin_arr.size() >= 2)
                        {
                            std::string location_type = origin_arr[0].value_or("");
                            int location_id = origin_arr[1].value_or(-1);

                            if (location_type == "module")
                            {
                                origin.type = LocationType::MODULE;
                            }
                            else if (location_type == "plane")
                            {
                                origin.type = LocationType::PLANE;
                            }
                            else
                            {
                                throw std::invalid_argument("Invalid origin location type: " + location_type);
                            }
                            origin.id = location_id;
                        }
                    }

                    // Parse destination
                    TubData destination{LocationType::MODULE, -1};
                    if (event_tbl.contains("destination") && event_tbl["destination"].is_array())
                    {
                        auto &dest_arr = *event_tbl["destination"].as_array();
                        if (dest_arr.size() == 2)
                        {
                            std::string location_type = dest_arr[0].value_or("");
                            int location_id = dest_arr[1].value_or(-1);

                            if (location_type == "module")
                            {
                                destination.type = LocationType::MODULE;
                            }
                            else if (location_type == "plane")
                            {
                                destination.type = LocationType::PLANE;
                            }
                            else
                            {
                                throw std::invalid_argument("Invalid destination location type: " + location_type);
                            }
                            destination.id = location_id;
                        }
                    }

                    if (time == -1.0 || id == -1 || origin.id == -1 || destination.id == -1)
                    {
                        throw std::invalid_argument("Tub event missing required fields");
                    }

                    result.push_back(std::make_unique<TubEvent>(time, id, origin, destination, safe, needs_security, parse_rfid(rfid_arr)));
                }
            }
            else if (group == "plane")
            {
                for (auto &&[event_key, event_value] : tbl)
                {
                    if (!event_value.is_table())
                        continue;

                    auto &event_tbl = *event_value.as_table();

                    int time = event_tbl["time"].value_or(-1.0);
                    int id = event_tbl["id"].value_or(-1);
                    int module = event_tbl["module"].value_or(-1);
                    int leaves_in = event_tbl["leaves_in"].value_or(-1);
                    toml::v3::array *rfid_arr = event_tbl["rfid"].as_array();

                    if (time == -1.0 || id == -1 || module == -1 || leaves_in == -1)
                    {
                        throw std::invalid_argument("Plane event missing required fields");
                    }

                    result.push_back(std::make_unique<PlaneEvent>(time, id, module, leaves_in, parse_rfid(rfid_arr)));
                }
            }
        }
    }

}