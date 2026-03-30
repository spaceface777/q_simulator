#ifndef MODULE_H
#define MODULE_H

#include <pthread.h>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <memory>
#include <thread>

#include "../packet/packet.h"
#include "../tub/tub.h"

namespace module
{

    class Module
    {
    private:
        pid_t pid_;
        uint8_t id_;
        bool running_ = false;

        uint8_t big_speed_;
        uint8_t small_speed_;
        uint16_t servo_degrees_;

    public:
        Module(uint8_t id)
        {
            id_ = id;
        }

        /**
         * @return The pid of this module.
         */
        pid_t pid() const noexcept
        {
            return pid_;
        }

        /**
         * @return The id of this module.
         */
        uint8_t id() const noexcept
        {
            return id_;
        }

        /**
         * @return Whether this module's executable is running.
         */
        bool is_running() const noexcept
        {
            return running_;
        }

        /**
         * @return The speed of the big belt.
         */
        uint8_t big_belt_speed() const noexcept
        {
            return big_speed_;
        }

        /**
         * @return The speed of the small belt.
         */
        uint8_t small_belt_speed() const noexcept
        {
            return small_speed_;
        }

        /**
         * @return The angle of the servo.
         */
        uint16_t servo_angle() const noexcept
        {
            return servo_degrees_;
        }

        /**
         * Run this module as the executable found at path.
         *
         * @param path The file to execute.
         */
        void run(std::string path);

        /**
         * Sends a packet to this module.
         *
         * @param packet The packet to send.
         */
        void send(const packet::Packet &packet);

        /**
         * Runs when an update packet has been sent from this module's simulation.
         *
         * @param packet The packet that was sent to this module's simulation.
         */
        void update(const packet::Update &packet);
    };

    /**
     * Adds a module.
     * @param ptr A unique_ptr to the module.
     */
    void add_module(std::unique_ptr<module::Module> ptr);

    /**
     * Gets an optional module from its id.
     * @param id The id.
     * @returns The module instance from the id.
     */
    std::optional<std::shared_ptr<module::Module>> get_module(uint8_t id);

    /**
     * Closes all module processes.
     */
    void stop();

}

#endif // MODULE_H