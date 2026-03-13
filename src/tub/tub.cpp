#include "tub.h"
#include "../module/module.h"
#include "../settings.h"

#include <thread>
#include <atomic>

namespace tub
{
    static std::atomic_bool stopped = false;

    void Tub::_run()
    {
        const std::optional<std::shared_ptr<module::Module>> module_opt = module::get_module(module_);
        if (!module_opt)
        {
            throw std::runtime_error("Failed to get module");
        }
        const std::shared_ptr<module::Module> module = module_opt.value();

        while (!stopped)
        {
            // assume if module is in T shape
            // then servo is aimed at right by default
            // angle > 90 = left
            // angle <= 90 = right

            if (location_progress_ > 100)
            {
                const id_t old_module = module_;
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
                    throw std::invalid_argument("Module " + std::to_string(old_module) + " and " + std::to_string(module_) + " are not connected in settings.toml");
                }

                location_progress_ = 100;
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
                    location_progress_ += module->small_belt_speed() / 100.0f;
                    break;
                case ModuleLocation::LEFT_BELT:
                    location_progress_ += -module->big_belt_speed() / 100.0f;
                    break;
                case ModuleLocation::RIGHT_BELT:
                    location_progress_ += module->big_belt_speed() / 100.0f;
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
            throw std::runtime_error("Double client instantiation");
        }

        thread_ptr_ = std::make_unique<std::thread>(&Tub::_run, this);
    }

    void stop()
    {
        stopped = true;
    }

    static std::map<uint8_t, std::unique_ptr<tub::Tub>> map;

    void add_tub(std::unique_ptr<tub::Tub> ptr)
    {
        if (map.count(ptr->id()))
        {
            throw std::invalid_argument("Attempted to double register tubs");
        }
        map[ptr->id()] = std::move(ptr);
    }

    std::optional<std::shared_ptr<tub::Tub>> get_tub(id_t id)
    {
        if (map.count(id))
        {
            return std::move(map.at(id));
        }
        return {};
    }

}