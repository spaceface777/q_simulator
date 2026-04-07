#ifndef VISUALIZATION_H
#define VISUALIZATION_H

#include <SDL.h>
#include <atomic>
#include <memory>
#include <vector>

namespace visualization
{

    /**
     * @brief Defines the possible rotations for a module.
     */
    enum class RotationState
    {
        ROT_0,
        ROT_90,
        ROT_180,
        ROT_270
    };

    class SimulationServer;

    /**
     * @brief Handles SDL2 visualization of the simulation.
     * Must be run on the main thread due to OS restrictions (Cocoa/macOS).
     */
    class Visualizer
    {
    public:
        Visualizer(SimulationServer *server);
        ~Visualizer();

        /**
         * @brief Runs the visualization loop. This call is blocking.
         */
        void run();

        /**
         * @brief Signals the visualization loop to stop.
         */
        void stop();

    private:
        /**
         * @brief Renders the state of a single module.
         * @param module_id The ID of the module.
         * @param x X-coordinate of the module visualization.
         * @param y Y-coordinate of the module visualization.
         */
        void render_module(int module_id, int x, int y);

        /**
         * @brief Renders the big belt for a module.
         * @param x X-coordinate of the module top-left.
         * @param y Y-coordinate of the module top-left.
         * @param speed Current speed of the belt.
         * @param is_running Whether the belt is running.
         * @param rotation The rotation of the module.
         */
        void render_big_belt(int x, int y, int speed, bool is_running, RotationState rotation);

        /**
         * @brief Renders the small belt for a module.
         * @param x X-coordinate of the module top-left.
         * @param y Y-coordinate of the module top-left.
         * @param speed Current speed of the belt.
         * @param is_running Whether the belt is running.
         * @param rotation The rotation of the module.
         */
        void render_small_belt(int x, int y, int speed, bool is_running, RotationState rotation);

        /**
         * @brief Renders the LED for a module.
         * @param module_x X-coordinate of the module top-left.
         * @param module_y Y-coordinate of the module top-left.
         * @param color packed RGBA color.
         * @param rotation The rotation of the module.
         */
        void render_led(int module_x, int module_y, uint32_t color, RotationState rotation);

        /**
         * @brief Renders the RFID reader for a module.
         * @param module_x X-coordinate of the module top-left.
         * @param module_y Y-coordinate of the module top-left.
         * @param rfid_state Current state of the RFID reader.
         * @param rotation The rotation of the module.
         */
        void render_rfid(int module_x, int module_y, int rfid_state, RotationState rotation);

        /**
         * @brief Renders the servo arm for a module.
         * @param module_x X-coordinate of the module top-left.
         * @param module_y Y-coordinate of the module top-left.
         * @param angle Current angle of the servo arm.
         * @param rotation The rotation of the module.
         */
        void render_servo(int module_x, int module_y, int angle, RotationState rotation);

        /**
         * @brief Renders a laser sensor for a module.
         * @param module_x X-coordinate of the module top-left.
         * @param module_y Y-coordinate of the module top-left.
         * @param power Whether the laser is powered on.
         * @param detection Whether the laser is currently detecting something.
         * @param is_left Whether this is the left laser.
         * @param rotation The rotation of the module.
         */
        void render_laser(int module_x, int module_y, int power, int detection, bool is_left, RotationState rotation);

        /**
         * @brief Renders all tubs in the simulation.
         */
        void render_tubs();

        SimulationServer *server_;
        std::atomic<bool> running_;

        SDL_Window *window_ = nullptr;
        SDL_Renderer *renderer_ = nullptr;

        // Animation state for belts
        double big_belt_offset_ = 0.0;
    };

} // namespace simulation

#endif // VISUALIZER_H
