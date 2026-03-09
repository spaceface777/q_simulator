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
            if (location_progress_ < 100)
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

                std::this_thread::sleep_for(TIMEOUT);
                continue;
            }

            switch (location_)
            {
            case ModuleLocation::BOTTOM_BELT:
                break;
            case ModuleLocation::LEFT_BELT:
                break;
            case ModuleLocation::RIGHT_BELT:
                break;
            }
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