#ifndef MODULE_H
#define MODULE_H

#include <pthread.h>
#include <cstdint>
#include <filesystem>
#include <vector>
#include <memory>
#include <thread>

#include "../message/message.h"
#include "../tub/tub.h"
#include "types.h"

namespace module
{

    class Module
    {
    private:
        pid_t pid_;
        ModuleId id_;
        bool running_ = false;       // whether the router process spawned by this module is running
        bool init_finished_ = false; // whether the router process is ready to receive packets

        BeltSpeed big_speed_;
        BeltSpeed small_speed_;
        ServoAngle servo_degrees_;
        LedColor led_color_;
        bool left_laser_active_ = false;
        bool right_laser_active_ = false;
        bool rfid_active_ = false;

    public:
        Module(uint8_t id)
        {
            id_ = ModuleId{id};
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
        ModuleId id() const noexcept
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
        BeltSpeed big_belt_speed() const noexcept
        {
            return big_speed_;
        }

        /**
         * @return The speed of the small belt.
         */
        BeltSpeed small_belt_speed() const noexcept
        {
            return small_speed_;
        }

        /**
         * @return The angle of the servo.
         */
        ServoAngle servo_angle() const noexcept
        {
            return servo_degrees_;
        }

        /**
         * @return The color of the led.
         */
        LedColor led_color() const noexcept
        {
            return led_color_;
        }

        /**
         * Sets the speed of the big belt.
         * @param speed The new speed for the big belt.
         */
        void set_big_belt_speed(BeltSpeed speed) noexcept
        {
            big_speed_ = speed;
        }

        /**
         * Sets the speed of the small belt.
         * @param speed The new speed for the small belt.
         */
        void set_small_belt_speed(BeltSpeed speed) noexcept
        {
            small_speed_ = speed;
        }

        /**
         * Sets the angle of the servo.
         * @param degrees The new angle for the servo.
         */
        void set_servo_angle(ServoAngle degrees) noexcept
        {
            servo_degrees_ = degrees;
        }

        /**
         * Sets the color of the led.
         * @param color The new color for the led.
         */
        void set_led_color(LedColor color) noexcept
        {
            led_color_ = color;
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
         * @return Whether this module has finished initialization.
         */
        bool is_initialization_finished() const noexcept
        {
            return init_finished_;
        }

        /**
         * Sets the initialization state of the router process.
         * @param active Whether this module's router process has finished initialization.
         */
        void set_initialization_finished(bool finished) noexcept
        {
            init_finished_ = finished;
        }

        /**
         * Run this module as the executable found at path.
         *
         * @param path The file to execute.
         */
        void run(std::string path);

        /**
         * Sends a message to this module.
         *
         * @param message The message to send.
         */
        void send(const message::Message &message);

    };

    /**
     * Adds a module.
     * @param ptr A unique_ptr to the module.
     */
    void add_module(std::unique_ptr<Module> ptr);

    /**
     * Gets an optional module from its id.
     * @param id The id.
     * @returns The module instance from the id.
     */
    std::optional<std::shared_ptr<Module>> get_module(ModuleId id);

    /**
     * @returns All modules.
     */
    std::vector<std::shared_ptr<Module>> get_modules();

    /**
     * Clears all modules.
     */
    void clear();

}

#endif // MODULE_H