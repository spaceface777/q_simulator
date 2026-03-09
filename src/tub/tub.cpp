#include "tub.h"
#include "../module/module.h"

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
                // transfer module
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

    void Tub::_stop()
    {
        stopped = true;
    }

}