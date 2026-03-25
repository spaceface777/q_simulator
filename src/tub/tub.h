#ifndef TUB_H
#define TUB_H

#include <cstdint>
#include <memory>
#include <thread>
#include <optional>

#include "../defs.h"

namespace tub
{

    class Tub
    {
    private:
        uint8_t id_;
        uint8_t module_;
        ModuleLocation location_;

        uint8_t target_module_;
        ModuleLocation target_module_location_;

        float location_progress_ = 0;
        std::unique_ptr<std::thread> thread_ptr_;

    public:
        Tub(uint8_t id, uint8_t module, uint8_t target_module, ModuleLocation target_module_location) : id_(id), module_(module), target_module_(target_module), target_module_location_(target_module_location)
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

        uint8_t target_module() const noexcept
        {
            return target_module_;
        }

        /**
         * @returns The target module location of the current tub.
         */
        ModuleLocation target_module_location() const noexcept
        {
            return target_module_location_;
        }

        /**
         * @returns A number between 0 and 100 indicating how far along the module location this tub is.
         * 0 = at center, 100 = at outskirts.
         */
        float module_location_progress() const noexcept
        {
            return location_progress_;
        }

    private:
        void _run();

    public:
        /**
         * Starts the update process of this tub.
         */
        void run();
    };

    /**
     * Stops all tubs.
     */
    void stop();

    /**
     * Adds a tub.
     * @param ptr A unique_ptr to the tub.
     */
    void add_tub(std::unique_ptr<tub::Tub> ptr);

    /**
     * Gets an optional tub from its id.
     * @param id The id.
     * @returns The tub instance from the id.
     */
    std::optional<std::shared_ptr<tub::Tub>> get_tub(uint8_t id);

    /**
     * Throws any exceptions that may have occurred during the execution of the thread.
     */
    void rethrow();

}

#endif