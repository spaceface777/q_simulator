#ifndef CLOCK_H
#define CLOCK_H

#include "../util.h"
#include <chrono>

namespace clock
{

    /**
     * Simulation clock.
     */
    class Clock 
    {
    private:
        std::chrono::milliseconds _virtual_time = std::chrono::milliseconds(0);
        std::chrono::time_point<std::chrono::steady_clock> _last_time_point;
        double _simulation_speed = 1.0;

    public:
        Clock() noexcept 
            : _last_time_point(std::chrono::steady_clock::now()) {}
    
        /**
         * Ticks the clock. 
         * Should occur at least as often as the time between any given simulation speed updates.
         */
        void tick() noexcept
        {
            auto now = std::chrono::steady_clock::now();
            auto real_time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_time_point);
            auto virtual_delta = std::chrono::duration_cast<std::chrono::milliseconds>(real_time_elapsed * _simulation_speed);
            _virtual_time += virtual_delta;
            _last_time_point = now;
        }

        /**
         * @return The virtual time in the simulation.
         */
        std::chrono::milliseconds get_virtual_time() const noexcept
        {
            return _virtual_time;
        }

        /**
         * @return The simulation speed.
         */
        double simulation_speed() const noexcept
        {
            return _simulation_speed;
        }

        /**
         * Sets the simulation speed.
         * @param speed The speed.
         * @throws If speed < 0.
         */
        void set_simulation_speed(double speed)
        {
            util::require(speed >= 0, "Invalid simulation speed in settings.yml");

            _simulation_speed = speed;
        }

    };

}

#endif