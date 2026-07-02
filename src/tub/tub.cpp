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
    static std::exception_ptr exception_ptr;
    static std::atomic_bool stopped = false;

    void Tub::_run()
    {
        const std::optional<std::shared_ptr<module::Module>> module_opt = module::get_module(module_);
        util::require(module_opt.has_value(), "Failed to get module");

        const std::shared_ptr<module::Module> module = module_opt.value();

        while (!stopped)
        {
            // assume if module is in T shape
            // then servo is aimed at right by default
            // angle > 90 = left
            // angle <= 90 = right

            if (location_progress_ > 1)
            {
                const uint8_t old_module = module_;
                const settings::ModuleSettings settings = settings::get_module_settings(module_);
                switch (location_)
                {
                case ModuleLocation::BOTTOM_BELT:
                    module_ = settings.bottom;
                    break;
                case ModuleLocation::LEFT_BELT:
                    module_ = settings.left;
                    break;
                case ModuleLocation::RIGHT_BELT:
                    module_ = settings.right;
                    break;
                }

                const settings::ModuleSettings other_settings = settings::get_module_settings(module_);

                if (other_settings.bottom == module_)
                {
                    location_ = ModuleLocation::BOTTOM_BELT;
                }
                else if (other_settings.left == module_)
                {
                    location_ = ModuleLocation::LEFT_BELT;
                }
                else if (other_settings.right == module_)
                {
                    location_ = ModuleLocation::RIGHT_BELT;
                }
                else
                {
                    stop();
                    throw std::invalid_argument("Module " + std::to_string(old_module) + " and " + std::to_string(module_) + " are not connected in settings.toml");
                }

                location_progress_ = 1;
            }
            else if (location_progress_ < 0)
            {
                switch (location_)
                {
                case ModuleLocation::BOTTOM_BELT:
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
                        location_ = ModuleLocation::BOTTOM_BELT;
                    }
                    break;
                case ModuleLocation::RIGHT_BELT:
                    if (module->servo_angle() > 170)
                    {
                        location_ = ModuleLocation::LEFT_BELT;
                    }
                    else
                    {
                        location_ = ModuleLocation::BOTTOM_BELT;
                    }
                    break;
                }
                location_progress_ = 0;
            }
            else
            {
                switch (location_)
                {
                case ModuleLocation::BOTTOM_BELT:
                    location_progress_ += settings::get_setting<double>("simulation_speed") * module->small_belt_speed() / 1000.0f;

                    if (in_rfid_ && (location_progress_ < 0.2 || location_progress_ > 0.8))
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(module_);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(id_) + " is owned by invalid module " + std::to_string(module_));
                        }

                        (*module)->send(packet::SimulationUpdate(module_, "rfid", false));
                        (*module)->set_rfid_active(false);
                        in_rfid_ = false;
                    }

                    if (!in_rfid_ && location_progress_ > 0.2 && location_progress_ < 0.8)
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(module_);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(id_) + " is owned by invalid module " + std::to_string(module_));
                        }

                        (*module)->send(packet::SimulationUpdate(module_, "rfid", true));
                        (*module)->set_rfid_active(true);
                        in_rfid_ = true;
                    }
                    break;
                case ModuleLocation::LEFT_BELT:
                    location_progress_ += settings::get_setting<double>("simulation_speed") * -module->big_belt_speed() / 1000.0f;

                    if (in_laser_ && (location_progress_ < 0.8 || location_progress_ > 1))
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(module_);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(id_) + " is owned by invalid module " + std::to_string(module_));
                        }

                        (*module)->send(packet::SimulationUpdate(module_, "left laser", false));
                        (*module)->set_left_laser_active(false);
                        in_laser_ = false;
                    }

                    if (!in_laser_ && location_progress_ > 0.8 && location_progress_ < 1)
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(module_);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(id_) + " is owned by invalid module " + std::to_string(module_));
                        }

                        (*module)->send(packet::SimulationUpdate(module_, "left laser", true));
                        (*module)->set_left_laser_active(true);
                        in_laser_ = true;
                    }
                    break;
                case ModuleLocation::RIGHT_BELT:
                    location_progress_ += settings::get_setting<double>("simulation_speed") * module->big_belt_speed() / 1000.0f;

                    if (in_laser_ && (location_progress_ < 0.8 || location_progress_ > 1))
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(module_);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(id_) + " is owned by invalid module " + std::to_string(module_));
                        }

                        (*module)->send(packet::SimulationUpdate(module_, "right laser", false));
                        (*module)->set_right_laser_active(false);
                        in_laser_ = false;
                    }

                    if (!in_laser_ && location_progress_ > 0.8 && location_progress_ < 1)
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(module_);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(id_) + " is owned by invalid module " + std::to_string(module_));
                        }

                        (*module)->send(packet::SimulationUpdate(module_, "right laser", true));
                        (*module)->set_right_laser_active(true);
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
        if (thread_ptr_)
        {
            throw std::runtime_error("Double tub instantiation");
        }

        thread_ptr_ = std::make_unique<std::thread>(
            [this]
            {
                try
                {
                    this->_run();
                }
                catch (...)
                {
                    exception_ptr = std::current_exception();
                }
            });

        std::cerr << "Started tub " << std::to_string(id_) << std::endl;
    }

    void stop()
    {
        stopped = true;
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

    void rethrow()
    {
        if (exception_ptr)
        {
            stop();
            std::cerr << "Rethrowing exception from tub thread" << std::endl;
            std::rethrow_exception(exception_ptr);
        }
    }
}