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
        const std::optional<std::shared_ptr<module::Module>> module_opt = module::get_module(_module);
        util::require(module_opt.has_value(), "Failed to get module");

        const std::shared_ptr<module::Module> module = module_opt.value();

        while (!stopped)
        {
            // assume if module is in T shape
            // then servo is aimed at right by default
            // angle > 90 = left
            // angle <= 90 = right

            std::cout << std::to_string(_location_progress) << std::endl;
            if (_location_progress > 1)
            {
                const uint8_t old_module = _module;
                const settings::ModuleSettings settings = settings::get_module_settings(_module);
                switch (_location)
                {
                case ModuleLocation::SMALL_BELT:
                    _module = settings.bottom;
                    break;
                case ModuleLocation::LEFT_BELT:
                    _module = settings.left;
                    break;
                case ModuleLocation::RIGHT_BELT:
                    _module = settings.right;
                    break;
                }

                const settings::ModuleSettings other_settings = settings::get_module_settings(_module);

                if (other_settings.bottom == _module)
                {
                    _location = ModuleLocation::SMALL_BELT;
                }
                else if (other_settings.left == _module)
                {
                    _location = ModuleLocation::LEFT_BELT;
                }
                else if (other_settings.right == _module)
                {
                    _location = ModuleLocation::RIGHT_BELT;
                }
                else
                {
                    stop();
                    throw std::invalid_argument("Module " + std::to_string(old_module) + " and " + std::to_string(_module) + " are not connected in settings.toml");
                }

                _location_progress = 1;
            }
            else if (_location_progress < 0)
            {
                switch (_location)
                {
                case ModuleLocation::SMALL_BELT:
                    if (module->servo_angle() > 90)
                    {
                        _location = ModuleLocation::RIGHT_BELT;
                    }
                    else
                    {
                        _location = ModuleLocation::LEFT_BELT;
                    }
                    break;
                case ModuleLocation::LEFT_BELT:
                    if (module->servo_angle() < 10)
                    {
                        _location = ModuleLocation::RIGHT_BELT;
                    }
                    else
                    {
                        _location = ModuleLocation::SMALL_BELT;
                    }
                    break;
                case ModuleLocation::RIGHT_BELT:
                    if (module->servo_angle() > 170)
                    {
                        _location = ModuleLocation::LEFT_BELT;
                    }
                    else
                    {
                        _location = ModuleLocation::SMALL_BELT;
                    }
                    break;
                }
                _location_progress = 0;
            }
            else
            {
                const double simulation_speed = settings::get_setting<double>("simulation_speed");

                switch (_location)
                {
                case ModuleLocation::SMALL_BELT:
                    _location_progress += simulation_speed * module->small_belt_speed() / 100.0 / 20.0 / 2.0;

                    if (_in_rfid && (_location_progress < 0.2 || _location_progress > 0.8))
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(_module);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(_id) + " is owned by invalid module " + std::to_string(_module));
                        }

                        (*module)->send(packet::SimulationUpdate(_module, "rfid", false));
                        (*module)->set_rfid_active(false);
                        _in_rfid = false;
                    }

                    if (!_in_rfid && _location_progress > 0.2 && _location_progress < 0.8)
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(_module);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(_id) + " is owned by invalid module " + std::to_string(_module));
                        }

                        (*module)->send(packet::SimulationUpdate(_module, "rfid", true));
                        (*module)->set_rfid_active(true);
                        _in_rfid = true;
                    }
                    break;
                case ModuleLocation::LEFT_BELT:
                    _location_progress += simulation_speed * -module->big_belt_speed() / 100.0 / 20.0 / 2.0;

                    if (_in_laser && (_location_progress < 0.8 || _location_progress > 1))
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(_module);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(_id) + " is owned by invalid module " + std::to_string(_module));
                        }

                        (*module)->send(packet::SimulationUpdate(_module, "left laser", false));
                        (*module)->set_left_laser_active(false);
                        _in_laser = false;
                    }

                    if (!_in_laser && _location_progress > 0.8 && _location_progress < 1)
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(_module);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(_id) + " is owned by invalid module " + std::to_string(_module));
                        }

                        (*module)->send(packet::SimulationUpdate(_module, "left laser", true));
                        (*module)->set_left_laser_active(true);
                        _in_laser = true;
                    }
                    break;
                case ModuleLocation::RIGHT_BELT:
                    _location_progress += simulation_speed * module->big_belt_speed() / 100.0 / 20.0 / 2.0;

                    if (_in_laser && (_location_progress < 0.8 || _location_progress > 1))
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(_module);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(_id) + " is owned by invalid module " + std::to_string(_module));
                        }

                        (*module)->send(packet::SimulationUpdate(_module, "right laser", false));
                        (*module)->set_right_laser_active(false);
                        _in_laser = false;
                    }

                    if (!_in_laser && _location_progress > 0.8 && _location_progress < 1)
                    {
                        const std::optional<std::shared_ptr<module::Module>> module = module::get_module(_module);
                        if (!module)
                        {
                            stop();
                            throw std::runtime_error("Tub " + std::to_string(_id) + " is owned by invalid module " + std::to_string(_module));
                        }

                        (*module)->send(packet::SimulationUpdate(_module, "right laser", true));
                        (*module)->set_right_laser_active(true);
                        _in_laser = true;
                    }
                    break;
                }
            }

            std::cout << "eepy time" << std::endl;
            std::this_thread::sleep_for(TIMEOUT);
        }
    }

    void Tub::run()
    {
        if (_thread_ptr)
        {
            throw std::runtime_error("Double tub instantiation");
        }

        _thread_ptr = std::make_unique<std::thread>(
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

        std::cerr << "Started tub " << std::to_string(_id) << std::endl;
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