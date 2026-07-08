#ifndef TUB_H
#define TUB_H

#include <cstdint>
#include <memory>
#include <thread>
#include <optional>
#include <vector>

#include "../defs.h"

namespace tub
{

    class Tub
    {
    private:
        uint8_t _id;
        uint8_t _module;
        ModuleLocation _location = ModuleLocation::SMALL_BELT;

        uint8_t _target_module;
        ModuleLocation _target_module_location;

        bool _in_laser = false;
        bool _in_rfid = false;
        double _location_progress = 0.5;
        std::unique_ptr<std::thread> _thread_ptr;

    public:
        Tub(uint8_t id, uint8_t module, uint8_t target_module, ModuleLocation target_module_location) : _id(id), _module(module), _target_module(target_module), _target_module_location(target_module_location)
        {
        }

        ~Tub()
        {
            if (_thread_ptr && _thread_ptr->joinable())
            {
                _thread_ptr->join();
            }
        }

        uint8_t id() const noexcept
        {
            return _id;
        }

        uint8_t module() const noexcept
        {
            return _module;
        }

        /**
         * @returns The module location of the current tub.
         */
        ModuleLocation module_location() const noexcept
        {
            return _location;
        }

        uint8_t target_module() const noexcept
        {
            return _target_module;
        }

        /**
         * @returns The target module location of the current tub.
         */
        ModuleLocation target_module_location() const noexcept
        {
            return _target_module_location;
        }

        /**
         * @returns A number between 0 and 1 indicating how far along the module location this tub is.
         * 0 = at center, 1 = at outskirts.
         */
        double module_location_progress() const noexcept
        {
            return _location_progress;
        }

    private:
        void _run();

    public:
        /**
         * Starts the update process of this tub.
         */
        void run();

        Tub(Tub&&) noexcept = default;
        Tub& operator=(Tub&&) noexcept = default;

        Tub(const Tub&) = delete;
        Tub& operator=(const Tub&) = delete;
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
     * @returns All tubs.
     */
    std::vector<std::shared_ptr<tub::Tub>> get_tubs();

    /**
     * Throws any exceptions that may have occurred during the execution of the thread.
     */
    void rethrow();

}

#endif