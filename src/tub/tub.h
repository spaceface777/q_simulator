#ifndef TUB_H
#define TUB_H

#include <cstdint>
#include <memory>
#include <thread>

#include "../defs.h"

namespace tub
{

    class Tub
    {
    private:
        uint8_t id_;
        uint8_t module_;
        ModuleLocation location_;
        float location_progress_ = 0;
        std::unique_ptr<std::thread> thread_ptr_;

    public:
        Tub(uint8_t id, uint8_t module) : id_(id), module_(module)
        {
        }

        ~Tub()
        {
            if (thread_ptr_ && thread_ptr_->joinable())
            {
                thread_ptr_->join();
            }
        }

        uint8_t id() const noexcept
        {
            return id_;
        }

        uint8_t module() const noexcept
        {
            return module_;
        }

        /**
         * @returns The module location of the current tub.
         */
        ModuleLocation module_location() const noexcept
        {
            return location_;
        }

        /**
         * @returns A number between 0 and 100 indicating how far along the module location this tub is.
         * Measured from the center to the outskirts.
         */
        float module_location_progress() const noexcept
        {
            return location_progress_;
        }

    private:
        void _run();

        void _stop();

    public:
        /**
         * Starts the update process of this tub.
         */
        void run();
    };
}

#endif