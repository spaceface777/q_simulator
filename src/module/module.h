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

        uint8_t big_speed_ = 0;
        uint8_t small_speed_ = 0;
        uint16_t servo_degrees_ = 0;
        int led_color_ = 0;
        bool left_laser_active_ = false;
        bool right_laser_active_ = false;
        bool rfid_active_ = false;

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
         * @return The color of the led.
         */
        int led_color() const noexcept
        {
            return led_color_;
        }

        /**
         * @return Whether the left laser is active.
         */
        bool is_left_laser_active() const noexcept
        {
            return left_laser_active_;
        }

        /**
         * Sets the state of the left laser.
         * @param active Whether it is active.
         */
        void set_left_laser_active(bool active) noexcept
        {
            left_laser_active_ = active;
        }

        /**
         * @return Whether the right laser is active.
         */
        bool is_right_laser_active() const noexcept
        {
            return right_laser_active_;
        }

        /**
         * Sets the state of the right laser.
         * @param active Whether it is active.
         */
        void set_right_laser_active(bool active) noexcept
        {
            right_laser_active_ = active;
        }

        /**
         * @return Whether the RFID reader is active.
         */
        bool is_rfid_active() const noexcept
        {
            return rfid_active_;
        }

        /**
         * Sets the state of the RFID reader.
         * @param active Whether it is active.
         */
        void set_rfid_active(bool active) noexcept
        {
            rfid_active_ = active;
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
        void update(const packet::ModuleUpdate packet);
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
     * @returns All modules.
     */
    std::vector<std::shared_ptr<module::Module>> get_modules();

}

#endif // MODULE_H