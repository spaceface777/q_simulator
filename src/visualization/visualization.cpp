// Visualization code written by Ali, adapted to code standards

#include "visualization.h"
#include <cmath>
#include <iostream>
#include <string>
#include <map>
#include <SDL.h>
#include <atomic>
#include <memory>
#include <vector>

#include "../module/module.h"
#include "../settings.h"

namespace visualization
{

    static std::atomic<bool> running_;

    static SDL_Window *window_ = nullptr;
    static SDL_Renderer *renderer_ = nullptr;

    static double big_belt_offset_ = 0.0;

    // Simple 3x5 pixel font for digits 0-9
    const uint16_t PIXEL_FONT[10] = {
        0x7B6F, // 0: 111, 101, 101, 101, 111
        0x2492, // 1: 010, 010, 010, 010, 010
        0x73E7, // 2: 111, 001, 111, 100, 111
        0x73CF, // 3: 111, 001, 111, 001, 111
        0x5BC9, // 4: 101, 101, 111, 001, 001
        0x79CF, // 5: 111, 100, 111, 001, 111
        0x79EF, // 6: 111, 100, 111, 101, 111
        0x7292, // 7: 111, 001, 010, 010, 010
        0x7BEF, // 8: 111, 101, 111, 101, 111
        0x7BCF  // 9: 111, 101, 111, 001, 111
    };

    static void draw_digit(SDL_Renderer *renderer, int x, int y, int digit, int scale)
    {
        if (digit < 0 || digit > 9)
        {
            return;
        }

        uint16_t bits = PIXEL_FONT[digit];
        for (int row = 0; row < 5; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                if (bits & (1 << (14 - (row * 3 + col))))
                {
                    SDL_Rect r = {x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(renderer, &r);
                }
            }
        }
    }

    static void draw_number(SDL_Renderer *renderer, int x, int y, int number, int scale)
    {
        std::string s = std::to_string(number);
        for (size_t i = 0; i < s.length(); ++i)
        {
            draw_digit(renderer, x + i * 4 * scale, y, s[i] - '0', scale);
        }
    }

    static void get_module_coords(int module_id, int &x, int &y)
    {
        static const int coords[19][2] = {
            {0, 0}, // padding
            {0, 4},
            {1, 4},
            {2, 4},
            {3, 4}, // 1-4
            {0, 3},
            {3, 3}, // 5-6
            {0, 2},
            {1, 2},
            {2, 2},
            {3, 2}, // 7-10
            {0, 1},
            {1, 1},
            {2, 1},
            {3, 1},
            {4, 1}, // 11-15
            {2, 0},
            {3, 0},
            {4, 0} // 16-18
        };
        int module_width = 180;
        int module_height = 180;
        int margin = 20;

        int col = 0;
        int row = 0;
        if (module_id >= 1 && module_id <= 18)
        {
            col = coords[module_id][0];
            row = coords[module_id][1];
        }
        x = margin + col * (module_width + margin);
        y = margin + row * (module_height + margin);
    }

    void init()
    {
        if (running_)
            return;
        running_ = true;

        if (SDL_Init(SDL_INIT_VIDEO) < 0)
        {
            std::cerr << "SDL could not initialize! SDL_Error: " << SDL_GetError() << std::endl;
            running_ = false;
            return;
        }

        window_ = SDL_CreateWindow("Quercus Airport Visualization",
                                   SDL_WINDOWPOS_UNDEFINED,
                                   SDL_WINDOWPOS_UNDEFINED,
                                   1020,
                                   1020,
                                   SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!window_)
        {
            std::cerr << "Window could not be created! SDL_Error: " << SDL_GetError() << std::endl;
            SDL_Quit();
            running_ = false;
            return;
        }

        renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!renderer_)
        {
            std::cerr << "Renderer could not be created! SDL_Error: " << SDL_GetError() << std::endl;
            SDL_DestroyWindow(window_);
            SDL_Quit();
            running_ = false;
            return;
        }
    }

    void update()
    {
        uint32_t last_time = SDL_GetTicks();

        if (!running_)
        {
            return;
        }

        SDL_Event e;
        while (SDL_PollEvent(&e) != 0)
        {
            if (e.type == SDL_QUIT)
            {
                running_ = false;
            }
        }

        uint32_t current_time = SDL_GetTicks();
        double dt = (current_time - last_time) / 1000.0;
        last_time = current_time;

        SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 255);
        SDL_RenderClear(renderer_);

        for (int i = 1; i <= settings::get_modules_active(); ++i)
        {
            int x, y;
            get_module_coords(i, x, y);
            render_module(i, x, y);
        }

        render_tubs();

        big_belt_offset_ += dt * 50.0;

        SDL_RenderPresent(renderer_);
        SDL_Delay(16);
    }

    void stop()
    {
        running_ = false;

        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    bool is_running()
    {
        return running_;
    }

    static void render_module(int module_id, int x, int y)
    {

        const std::optional<std::shared_ptr<module::Module>> module_opt = module::get_module(module_id);
        if (!module_opt)
        {
            return;
        }

        const std::shared_ptr<module::Module> module = module_opt.value();
        const settings::ModuleSettings module_settings = settings::get_module_settings(module_id);

        // Draw module background
        SDL_Rect bg = {x, y, 180, 180};
        SDL_SetRenderDrawColor(renderer_, 60, 60, 60, 255);
        SDL_RenderFillRect(renderer_, &bg);

        // Draw module ID border
        SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
        SDL_RenderDrawRect(renderer_, &bg);

        // Draw module ID text
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        draw_number(renderer_, x + 5, y + 5, module_id, 2);

        // Render Big Belt
        render_big_belt(x, y, module->big_belt_speed(), module->big_belt_speed() != 0, module_settings.angle);

        // Render Small Belt
        render_small_belt(x, y, module->small_belt_speed(), module->small_belt_speed() != 0, module_settings.angle);

        // Render LED
        render_led(x, y, module->led_color(), module_settings.angle);

        // Render RFID
        render_rfid(x, y, module->is_rfid_active(), module_settings.angle);

        // Render Servo
        render_servo(x, y, module->servo_angle(), module_settings.angle);

        // Render Lasers
        render_laser(x, y, module->is_left_laser_active(), true, true, module_settings.angle);
        render_laser(x, y, module->is_right_laser_active(), true, false, module_settings.angle);
    }

    static void render_led(int module_x, int module_y, uint32_t color, int rotation)
    {
        int led_rel_x, led_rel_y;
        int offset = 20;
        switch (rotation)
        {
        case 0:
            led_rel_x = 90 + offset;
            led_rel_y = 90 + offset;
            break;
        case 90:
            led_rel_x = 90 - offset;
            led_rel_y = 90 + offset;
            break;
        case 180:
            led_rel_x = 90 - offset;
            led_rel_y = 90 - offset;
            break;
        case 270:
            led_rel_x = 90 + offset;
            led_rel_y = 90 - offset;
            break;

        default:
            throw std::invalid_argument("Invalid module angle " + std::to_string(rotation));
        }

        int x = module_x + led_rel_x;
        int y = module_y + led_rel_y;

        uint8_t r = color & 0xFF;
        uint8_t g = (color >> 8) & 0xFF;
        uint8_t b = (color >> 16) & 0xFF;
        uint8_t a = (color >> 24) & 0xFF;
        if (a == 0)
            a = 255; // Ensure visibility for 0xRRGGBB colors

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, r, g, b, a);

        // Draw a small circle
        int radius = 6;
        for (int w = -radius; w <= radius; w++)
        {
            for (int h = -radius; h <= radius; h++)
            {
                if (w * w + h * h <= radius * radius)
                {
                    SDL_RenderDrawPoint(renderer_, x + w, y + h);
                }
            }
        }

        // Add a small highlight for a 3D effect
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 150);
        SDL_RenderDrawPoint(renderer_, x - 2, y - 2);
    }

    static void render_rfid(int module_x, int module_y, int rfid_state, int rotation)
    {
        if (rfid_state == 0)
            return; // Invisible when IDLE

        // Blinking logic: 500ms ON, 500ms OFF
        if ((SDL_GetTicks() / 500) % 2 == 0)
            return;

        int rfid_rel_x, rfid_rel_y;
        // Small belt center coordinates:
        // ROT_0: (90, 142)
        // ROT_90: (37, 90)
        // ROT_180: (90, 37)
        // ROT_270: (142, 90)
        switch (rotation)
        {
        case 0:
            rfid_rel_x = 90;
            rfid_rel_y = 142;
            break;
        case 90:
            rfid_rel_x = 37;
            rfid_rel_y = 90;
            break;
        case 180:
            rfid_rel_x = 90;
            rfid_rel_y = 37;
            break;
        case 270:
            rfid_rel_x = 142;
            rfid_rel_y = 90;
            break;
        default:
            throw std::invalid_argument("Invalid module angle " + std::to_string(rotation));
        }

        SDL_Rect rfid_rect = {module_x + rfid_rel_x - 10, module_y + rfid_rel_y - 10, 20, 20};
        SDL_SetRenderDrawColor(renderer_, 255, 0, 0, 255); // Red
        SDL_RenderFillRect(renderer_, &rfid_rect);
    }

    static void render_big_belt(int module_x, int module_y, int speed, bool is_running, int rotation)
    {
        int width, height, belt_x, belt_y;

        switch (rotation)
        {
        case 0:
        case 180:
            width = 180;
            height = 30;
            belt_x = module_x;
            belt_y = module_y + 75;
            break;
        case 90:
        case 270:
            width = 30;
            height = 180;
            belt_x = module_x + 75;
            belt_y = module_y;
            break;

        default:
            throw std::invalid_argument("Invalid module angle " + std::to_string(rotation));
        };

        // Belt surface
        SDL_Rect belt = {belt_x, belt_y, width, height};

        if (is_running)
        {
            SDL_SetRenderDrawColor(renderer_, 34, 139, 34, 255); // Forest Green
        }
        else
        {
            SDL_SetRenderDrawColor(renderer_, 40, 40, 40, 255); // Dark Gray
        }
        SDL_RenderFillRect(renderer_, &belt);

        // Belt border
        if (is_running)
        {
            SDL_SetRenderDrawColor(renderer_, 50, 205, 50, 255); // Lime Green
        }
        else
        {
            SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
        }
        SDL_RenderDrawRect(renderer_, &belt);

        // Animation lines
        if (is_running && speed != 0)
        {
            SDL_SetRenderDrawColor(renderer_, 144, 238, 144, 150); // Light Green
            double step = 20.0;
            double offset = fmod(big_belt_offset_ * (speed / 100.0), step);
            int longer_dimension = height > width ? height : width;
            for (double l = -step; l < longer_dimension + step; l += step)
            {
                switch (rotation)
                {
                case 0:
                case 180:
                {
                    int draw_x = belt_x + (rotation == 0 ? static_cast<int>(l + offset)
                                                         : width - static_cast<int>(l + offset));
                    if (draw_x >= belt_x && draw_x < belt_x + width)
                    {
                        SDL_RenderDrawLine(renderer_, draw_x, belt_y, draw_x, belt_y + height - 1);
                    };
                    break;
                }
                case 90:
                case 270:
                {
                    int draw_y = belt_y + (rotation == 90 ? static_cast<int>(l + offset)
                                                          : height - static_cast<int>(l + offset));
                    if (draw_y >= belt_y && draw_y < belt_y + height)
                    {
                        SDL_RenderDrawLine(renderer_, belt_x, draw_y, belt_x + width - 1, draw_y);
                    };
                    break;
                }
                default:
                    throw std::invalid_argument("Invalid module angle " + std::to_string(rotation));
                }
            }
        }
    }

    static void render_small_belt(int module_x, int module_y, int speed, bool is_running, int rotation)
    {
        int width, height, belt_x, belt_y;

        switch (rotation)
        {
        case 0:
            belt_x = module_x + 75;
            belt_y = module_y + 105;
            width = 30;
            height = 75;
            break;
        case 90:
            belt_x = module_x;
            belt_y = module_y + 75;
            width = 75;
            height = 30;
            break;
        case 180:
            belt_x = module_x + 75;
            belt_y = module_y;
            width = 30;
            height = 75;
            break;
        case 270:
            belt_x = module_x + 105;
            belt_y = module_y + 75;
            width = 75;
            height = 30;
            break;

        default:
            throw std::invalid_argument("Invalid module angle " + std::to_string(rotation));
        }

        // Belt surface
        SDL_Rect belt = {belt_x, belt_y, width, height};

        if (is_running)
        {
            SDL_SetRenderDrawColor(renderer_, 34, 139, 34, 255); // Forest Green
        }
        else
        {
            SDL_SetRenderDrawColor(renderer_, 40, 40, 40, 255); // Dark Gray
        }
        SDL_RenderFillRect(renderer_, &belt);

        // Belt border
        if (is_running)
        {
            SDL_SetRenderDrawColor(renderer_, 50, 205, 50, 255); // Lime Green
        }
        else
        {
            SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
        }
        SDL_RenderDrawRect(renderer_, &belt);

        // Animation lines
        if (is_running && speed != 0)
        {
            SDL_SetRenderDrawColor(renderer_, 144, 238, 144, 150); // Light Green
            double step = 20.0;
            double offset = -fmod(big_belt_offset_ * (speed / 100.0), step);
            int longer_dimension = height > width ? height : width;
            for (double l = -step; l < longer_dimension + step; l += step)
            {
                switch (rotation)
                {
                case 0:
                case 180:
                {
                    int draw_y = belt_y + (rotation == 0 ? static_cast<int>(l + offset)
                                                         : height - static_cast<int>(l + offset));
                    if (draw_y >= belt_y && draw_y < belt_y + height)
                    {
                        SDL_RenderDrawLine(renderer_, belt_x, draw_y, belt_x + width - 1, draw_y);
                    }
                    break;
                }
                case 90:
                case 270:
                {
                    int draw_x = belt_x + (rotation == 270 ? static_cast<int>(l + offset)
                                                           : width - static_cast<int>(l + offset));
                    if (draw_x >= belt_x && draw_x < belt_x + width)
                    {
                        SDL_RenderDrawLine(renderer_, draw_x, belt_y, draw_x, belt_y + height - 1);
                    }
                    break;
                }

                default:
                    throw std::invalid_argument("Invalid module angle " + std::to_string(rotation));
                }
            }
        }
    }

    static void render_laser(
        int module_x, int module_y, int power, int detection, bool is_left, int rotation)
    {
        if (power == 0)
            return;

        // Blinking logic if detecting: 100ms ON, 100ms OFF
        if (detection != 0)
        {
            if ((SDL_GetTicks() / 100) % 2 == 0)
                return;
        }

        int x1, y1, x2, y2;
        int margin = 36; // 20% of 180

        switch (rotation)
        {
        case 0:
            x1 = x2 = module_x + (is_left ? margin : 180 - margin);
            y1 = module_y + 75;
            y2 = module_y + 105;
            break;
        case 90:
            x1 = module_x + 75;
            x2 = module_x + 105;
            y1 = y2 = module_y + (is_left ? margin : 180 - margin);
            break;
        case 180:
            x1 = x2 = module_x + (is_left ? 180 - margin : margin);
            y1 = module_y + 75;
            y2 = module_y + 105;
            break;
        case 270:
            x1 = module_x + 75;
            x2 = module_x + 105;
            y1 = y2 = module_y + (is_left ? 180 - margin : margin);
            break;

        default:
            throw std::invalid_argument("Invalid module angle " + std::to_string(rotation));
        }

        SDL_SetRenderDrawColor(renderer_, 255, 0, 0, 255); // Red
        int thickness = 2;
        SDL_Rect beam;
        if (x1 == x2)
        {
            beam = {x1 - thickness / 2, y1, thickness, y2 - y1};
        }
        else
        {
            beam = {x1, y1 - thickness / 2, x2 - x1, thickness};
        }
        SDL_RenderFillRect(renderer_, &beam);

        // Draw small sensor dots at the ends of the laser line
        SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
        SDL_Rect dot1 = {x1 - 2, y1 - 2, 4, 4};
        SDL_Rect dot2 = {x2 - 2, y2 - 2, 4, 4};
        SDL_RenderFillRect(renderer_, &dot1);
        SDL_RenderFillRect(renderer_, &dot2);
    }

    static void render_servo(int module_x, int module_y, int angle, int rotation)
    {
        // Pivot at (90, 75) relative to module (top-middle of big belt)
        double pivot_rel_x = 90.0;
        double pivot_rel_y = 75.0;

        // Base angle depends on rotation
        double base_angle_rad = 0.0;
        switch (rotation)
        {
        case 0:
            base_angle_rad = 0.0;
            break;
        case 90:
            base_angle_rad = M_PI / 2.0;
            pivot_rel_x = 105.0;
            pivot_rel_y = 90.0;
            break;
        case 180:
            base_angle_rad = M_PI;
            pivot_rel_x = 90.0;
            pivot_rel_y = 105.0;
            break;
        case 270:
            base_angle_rad = 3.0 * M_PI / 2.0;
            pivot_rel_x = 75.0;
            pivot_rel_y = 90.0;
            break;

        default:
            throw std::invalid_argument("Invalid module angle " + std::to_string(rotation));
        }

        // angle 0: parallel to big belt (right), 90: perpendicular (towards small belt)
        double servo_angle_rad = (angle / 180.0) * M_PI;
        double total_angle_rad = base_angle_rad + servo_angle_rad;

        double length = 35.0;
        double end_rel_x = pivot_rel_x + length * cos(total_angle_rad);
        double end_rel_y = pivot_rel_y + length * sin(total_angle_rad);

        SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255); // Light Gray
        int x1 = module_x + static_cast<int>(pivot_rel_x);
        int y1 = module_y + static_cast<int>(pivot_rel_y);
        int x2 = module_x + static_cast<int>(end_rel_x);
        int y2 = module_y + static_cast<int>(end_rel_y);

        // Draw thick line (width 4)
        for (int i = -2; i <= 1; ++i)
        {
            if (abs(x2 - x1) > abs(y2 - y1))
            {
                SDL_RenderDrawLine(renderer_, x1, y1 + i, x2, y2 + i);
            }
            else
            {
                SDL_RenderDrawLine(renderer_, x1 + i, y1, x2 + i, y2);
            }
        }

        // Draw the pivot point
        SDL_SetRenderDrawColor(renderer_, 50, 50, 50, 255);
        SDL_Rect pivot_rect = {x1 - 4, y1 - 4, 8, 8};
        SDL_RenderFillRect(renderer_, &pivot_rect);
    }

    static void render_tubs()
    {
        for (const std::shared_ptr<tub::Tub> &tub : tub::get_tubs())
        {
            std::optional<std::shared_ptr<module::Module>> module = module::get_module(tub->module());
            if (!module)
            {
                continue;
            }

            const settings::ModuleSettings module_settings = settings::get_module_settings((*module)->id());

            int module_x, module_y;
            get_module_coords(tub->module(), module_x, module_y);

            double rel_x = 0, rel_y = 0;
            if (tub->module_location() == ModuleLocation::LEFT_BELT || tub->module_location() == ModuleLocation::RIGHT_BELT)
            {
                switch (module_settings.angle)
                {
                case 0:
                    rel_x = tub->module_location_progress() * 180.0;
                    rel_y = 90.0;
                    break;
                case 90:
                    rel_x = 90.0;
                    rel_y = tub->module_location_progress() * 180.0;
                    break;
                case 180:
                    rel_x = (1.0 - tub->module_location_progress()) * 180.0;
                    rel_y = 90.0;
                    break;
                case 270:
                    rel_x = 90.0;
                    rel_y = (1.0 - tub->module_location_progress()) * 180.0;
                    break;

                default:
                    throw std::invalid_argument("Invalid module angle " + std::to_string(module_settings.angle));
                }
            }
            else
            {
                switch (module_settings.angle)
                {
                case 0:
                    rel_x = 90.0;
                    rel_y = 105.0 + tub->module_location_progress() * 75.0;
                    break;
                case 90:
                    rel_x = 75.0 - tub->module_location_progress() * 75.0;
                    rel_y = 90.0;
                    break;
                case 180:
                    rel_x = 90.0;
                    rel_y = 75.0 - tub->module_location_progress() * 75.0;
                    break;
                case 270:
                    rel_x = 105.0 + tub->module_location_progress() * 75.0;
                    rel_y = 90.0;
                    break;

                default:
                    throw std::invalid_argument("Invalid module angle " + std::to_string(module_settings.angle));
                }
            }

            int x = module_x + (int)rel_x;
            int y = module_y + (int)rel_y;

            // Draw tub (blue circle)
            SDL_SetRenderDrawColor(renderer_, 255, 192, 203, 255);
            int radius = 10;
            for (int w = -radius; w <= radius; w++)
            {
                for (int h = -radius; h <= radius; h++)
                {
                    if (w * w + h * h <= radius * radius)
                    {
                        SDL_RenderDrawPoint(renderer_, x + w, y + h);
                    }
                }
            }

            // Draw tub ID
            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
            draw_number(renderer_, x - 4, y - 6, tub->id(), 1);
        }
    }

}
