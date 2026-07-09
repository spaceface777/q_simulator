#include "tub.h"
#include "../module/module.h"
#include "../settings.h"
#include "../packet/packet.h"
#include "../util.h"

#include <thread>
#include <atomic>
#include <iostream>

namespace tub
{

    static constexpr double RFID_LOCATION = 0.5;
    static constexpr double RFID_RANGE = 0.1;

    static constexpr double LASER_LOCATION = 0.75;
    static constexpr double LASER_RANGE = 0.1;

    void Tub::run_()
    {
        while (!stopped_)
        {
            // assume if module is in T shape
            // then servo is aimed at right by default
            // angle > 90 = left
            // angle <= 90 = right

            const std::optional<std::shared_ptr<module::Module>> module_opt = module::get_module(module_);
            if (!module_opt)
            {
                stop();
                throw std::runtime_error("Tub " + std::to_string(id_) + " is owned by invalid module " + std::to_string(module_));
            }
            const std::shared_ptr<module::Module> module = module_opt.value();
            const settings::ModuleSettings settings = settings::get_module_settings(module_);

            if (location_progress_ > 1)
            {
                const uint8_t old_module = module_;
                uint8_t new_module = -1;
                switch (location_)
                {
                case ModuleLocation::SMALL_BELT:
                    new_module = settings.bottom;
                    break;
                case ModuleLocation::LEFT_BELT:
                    new_module = settings.left;
                    break;
                case ModuleLocation::RIGHT_BELT:
                    new_module = settings.right;
                    break;
                }

                const settings::ModuleSettings new_settings = settings::get_module_settings(new_module);
                if (new_settings.bottom == old_module)
                {
                    location_ = ModuleLocation::SMALL_BELT;
                }
                else if (new_settings.left == old_module)
                {
                    location_ = ModuleLocation::LEFT_BELT;
                }
                else if (new_settings.right == old_module)
                {
                    location_ = ModuleLocation::RIGHT_BELT;
                }
                else
                {
                    stop();
                    throw std::invalid_argument("Module " + std::to_string(old_module) + " and " + std::to_string(new_module) + " are not connected in settings.toml");
                }

                location_progress_ = 1;
                module_ = new_module;
            }
            else if (location_progress_ < 0)
            {
                switch (location_)
                {
                case ModuleLocation::SMALL_BELT:
                    if (module->servo_angle() > 90)
                    {
                        location_ = ModuleLocation::RIGHT_BELT;
                    }
                    else
                    {
                        location_ = ModuleLocation::LEFT_BELT;
                    }
                    break;
                case ModuleLocation::LEFT_BELT:
                    if (module->servo_angle() < 10)
                    {
                        location_ = ModuleLocation::RIGHT_BELT;
                    }
                    else
                    {
                        location_ = ModuleLocation::SMALL_BELT;
                    }
                    break;
                case ModuleLocation::RIGHT_BELT:
                    if (module->servo_angle() > 170)
                    {
                        location_ = ModuleLocation::LEFT_BELT;
                    }
                    else
                    {
                        location_ = ModuleLocation::SMALL_BELT;
                    }
                    break;
                }
                location_progress_ = 0;
            }
            else
            {
                const double simulation_speed = settings::get_setting<double>("simulation_speed");

                switch (location_)
                {
                case ModuleLocation::SMALL_BELT:
                    location_progress_ += simulation_speed * module->small_belt_speed() / 100.0 / 60.0 / 4.0;

                    if (in_rfid_ && (location_progress_ < RFID_LOCATION - RFID_RANGE || location_progress_ > RFID_LOCATION + RFID_RANGE))
                    {
                        module->send(packet::SimulationUpdate(module_, "rfid", false));
                        module->set_rfid_active(false);
                        in_rfid_ = false;
                    }
                    else if (!in_rfid_ && location_progress_ > RFID_LOCATION - RFID_RANGE && location_progress_ < RFID_LOCATION + RFID_RANGE)
                    {
                        module->send(packet::SimulationUpdate(module_, "rfid", true));
                        module->set_rfid_active(true);
                        in_rfid_ = true;
                    }
                    break;
                case ModuleLocation::LEFT_BELT:
                    location_progress_ += simulation_speed * module->big_belt_speed() / 100.0 / 60.0 / 4.0;

                    if (in_laser_ && (location_progress_ < LASER_LOCATION - LASER_RANGE || location_progress_ > LASER_LOCATION + LASER_RANGE))
                    {
                        module->send(packet::SimulationUpdate(module_, "left laser", false));
                        module->set_left_laser_active(false);
                        in_laser_ = false;
                    }
                    else if (!in_laser_ && location_progress_ > LASER_LOCATION - LASER_RANGE && location_progress_ < LASER_LOCATION + LASER_RANGE)
                    {

                        module->send(packet::SimulationUpdate(module_, "left laser", true));
                        module->set_left_laser_active(true);
                        in_laser_ = true;
                    }
                    break;
                case ModuleLocation::RIGHT_BELT:
                    location_progress_ += simulation_speed * -module->big_belt_speed() / 100.0 / 60.0 / 4.0;

                    if (in_laser_ && (location_progress_ < LASER_LOCATION - LASER_RANGE || location_progress_ > LASER_LOCATION + LASER_RANGE))
                    {
                        module->send(packet::SimulationUpdate(module_, "right laser", false));
                        module->set_right_laser_active(false);
                        in_laser_ = false;
                    }
                    else if (!in_laser_ && location_progress_ > LASER_LOCATION - LASER_RANGE && location_progress_ < LASER_LOCATION + LASER_RANGE)
                    {
                        module->send(packet::SimulationUpdate(module_, "right laser", true));
                        module->set_right_laser_active(true);
                        in_laser_ = true;
                    }
                    break;
                }
            }

            std::this_thread::sleep_for(TIMEOUT);
        }
    }

    void Tub::run()
    {
        util::require(!stopped_, "Cannot run the same tub twice");

        tub_future_ = std::async(std::launch::async, [this]
                                 { run_(); });
    }

    void Tub::check()
    {
        if (!tub_future_.valid())
        {
            return;
        }

        auto status = tub_future_.wait_for(std::chrono::seconds(0));
        if (status == std::future_status::ready)
        {
            tub_future_.get();
        }
    }

    void Tub::stop()
    {
        stopped_ = true;
    }

    void stop()
    {
        for (auto &tub : get_tubs())
        {
            tub->stop();
        }
    }

    static std::map<uint8_t, std::shared_ptr<tub::Tub>> map;

    void add_tub(std::unique_ptr<tub::Tub> ptr)
    {
        if (map.count(ptr->id()))
        {
            stop();
            throw std::invalid_argument("Attempted to double register tubs");
        }
        map[ptr->id()] = std::move(ptr);
    }

    std::optional<std::shared_ptr<tub::Tub>> get_tub(uint8_t id)
    {
        if (map.count(id))
        {
            return map.at(id);
        }
        return {};
    }

    std::vector<std::shared_ptr<tub::Tub>> get_tubs()
    {
        std::vector<std::shared_ptr<tub::Tub>> tubs;
        for (const auto &pair : map)
        {
            tubs.push_back(pair.second);
        }
        return tubs;
    }

    void check()
    {
        for (auto &tub : get_tubs())
        {
            tub->check();
        }
    }
}