// Visualization code written by Ali, adapted to code standards

#include "visualization.h"
#include <cmath>
#include <iostream>
#include <string>
#include <map>
#include <atomic>
#include <memory>
#include <vector>
#include <sstream>
#include <iomanip>
#include <functional>

#include "../module/module.h"
#include "../module/types.h"
#include "../defs.h"
#include "../settings.h"
#include "../synchronization/virtual_clock.h"
#include "buttons.h"

namespace visualization
{

    /**
     * Default scaling is kind of large, so reduce by a factor.
     */
    static constexpr double SCALING_FACTOR = 0.7;

    /**
     * Belt movement reduction.
     */
    static constexpr double BELT_FACTOR = 0.4;

    /**
     * The current scale.
     */
    static double scale = 1.0;

    /**
     * Scales a constant by the current scaling.
     * @param x The constant.
     * @return The scaled constant.
     */
    int scale_within_module(int x)
    {
        return (int)(x * SCALING_FACTOR * scale);
    }

    static std::atomic<bool> running_;

    static SDL_Window *window_ = nullptr;
    static SDL_Renderer *renderer_ = nullptr;

    static double belt_offset_ = 0.0;

    struct Glyph
    {
        GlyphId id;
        uint16_t bitmap;
    };

    class PixelFont
    {
    public:
        static constexpr uint8_t WIDTH = 3;
        static constexpr uint8_t HEIGHT = 5;

        static constexpr std::array<Glyph, static_cast<size_t>(GlyphId::COUNT)> DATA = {
            {{GlyphId::ZERO, 0x7B6F},  // 111, 101, 101, 101, 111
             {GlyphId::ONE, 0x2492},   // 010, 010, 010, 010, 010
             {GlyphId::TWO, 0x73E7},   // 111, 001, 111, 100, 111
             {GlyphId::THREE, 0x73CF}, // 111, 001, 111, 001, 111
             {GlyphId::FOUR, 0x5BC9},  // 101, 101, 111, 001, 001
             {GlyphId::FIVE, 0x79CF},  // 111, 100, 111, 001, 111
             {GlyphId::SIX, 0x79EF},   // 111, 100, 111, 101, 111
             {GlyphId::SEVEN, 0x7292}, // 111, 001, 010, 010, 010
             {GlyphId::EIGHT, 0x7BEF}, // 111, 101, 111, 101, 111
             {GlyphId::NINE, 0x7BCF},  // 111, 101, 111, 001, 111
             {GlyphId::D, 0b110101101101110},
             {GlyphId::B, 0b110101110101110},
             {GlyphId::G, 0b111100101101111},
             {GlyphId::DOT, 0b000000000000010},
             {GlyphId::MINUS, 0b000000111000000},
             {GlyphId::PLUS, 0b000010111010000},
             {GlyphId::X, 0b000101010101000},
             {GlyphId::LEFT, 0b001010100010001},
             {GlyphId::RIGHT, 0b100010001010100},
             {GlyphId::PAUSE, 0b101101101101101}}};

        static constexpr uint16_t get_bitmap(GlyphId id)
        {
            size_t index = static_cast<size_t>(id);
            if (index < DATA.size())
                return DATA[index].bitmap;
            throw std::invalid_argument("Invalid glyph id");
        }

        static constexpr uint16_t get_bitmap_from_char(char c)
        {
            if (c >= '0' && c <= '9')
                return get_bitmap(static_cast<GlyphId>(c - '0'));
            else if (c == 'd')
                return get_bitmap(GlyphId::D);
            else if (c == 'b')
                return get_bitmap(GlyphId::B);
            else if (c == 'g')
                return get_bitmap(GlyphId::G);
            else if (c == 'x')
                return get_bitmap(GlyphId::X);
            else if (c == '.')
                return get_bitmap(GlyphId::DOT);
            else if (c == '-')
                return get_bitmap(GlyphId::MINUS);
            else if (c == '+')
                return get_bitmap(GlyphId::PLUS);
            else if (c == '<')
                return get_bitmap(GlyphId::LEFT);
            else if (c == '>')
                return get_bitmap(GlyphId::RIGHT);

            throw std::invalid_argument("Invalid char");
        }
    };

    void draw_char(SDL_Renderer *renderer, int x, int y, char digit, int scale)
    {
        uint16_t bits = PixelFont::get_bitmap_from_char(digit);
        for (int row = 0; row < PixelFont::HEIGHT; ++row)
        {
            for (int col = 0; col < PixelFont::WIDTH; ++col)
            {
                if (bits & (1 << (14 - (row * PixelFont::WIDTH + col))))
                {
                    SDL_Rect r = {x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(renderer, &r);
                }
            }
        }
    }

    void draw_char(SDL_Renderer *renderer, int x, int y, GlyphId glyph, int scale)
    {
        uint16_t bits = PixelFont::get_bitmap(glyph);
        for (int row = 0; row < PixelFont::HEIGHT; ++row)
        {
            for (int col = 0; col < PixelFont::WIDTH; ++col)
            {
                if (bits & (1 << (14 - (row * PixelFont::WIDTH + col))))
                {
                    SDL_Rect r = {x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(renderer, &r);
                }
            }
        }
    }

    void draw_string(SDL_Renderer *renderer, int x, int y, std::string str, int scale)
    {
        for (size_t i = 0; i < str.length(); ++i)
        {
            draw_char(renderer, x + i * 4 * scale, y, str[i], scale);
        }
    }

    static void draw_number(SDL_Renderer *renderer, int x, int y, int number, int scale)
    {
        draw_string(renderer, x, y, std::to_string(number), scale);
    }

    static void draw_number(SDL_Renderer *renderer, int x, int y, double number, int scale)
    {
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1) << number;
        draw_string(renderer, x, y, ss.str(), scale);
    }

    static void get_module_coords(module::ModuleId id, int &x, int &y)
    {
        const int margin = scale_within_module(20);
        const settings::ModuleSettings settings = settings::get_module_settings(id);

        x = margin + settings.x * (scale_within_module(180) + margin);
        y = margin + settings.y * (scale_within_module(180) + margin);
    }

    static void render_led(int module_x, int module_y, module::LedColor color, settings::ModuleAngle rotation)
    {
        int led_rel_x, led_rel_y;
        int offset = scale_within_module(20);
        switch (rotation)
        {
        case settings::ModuleAngle::NORMAL:
            led_rel_x = scale_within_module(90) + offset;
            led_rel_y = scale_within_module(90) + offset;
            break;
        case settings::ModuleAngle::ROTATED_90:
            led_rel_x = scale_within_module(90) - offset;
            led_rel_y = scale_within_module(90) + offset;
            break;
        case settings::ModuleAngle::UPSIDE_DOWN:
            led_rel_x = scale_within_module(90) - offset;
            led_rel_y = scale_within_module(90) - offset;
            break;
        case settings::ModuleAngle::ROTATED_270:
            led_rel_x = scale_within_module(90) + offset;
            led_rel_y = scale_within_module(90) - offset;
            break;
        }

        int x = module_x + led_rel_x;
        int y = module_y + led_rel_y;

        SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(renderer_, color.r(), color.g(), color.b(), color.a() == 0 ? 255 : color.a());

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

        if (settings::get_setting<bool>("debug"))
        {
            draw_number(renderer_, x + 5, y + 10, color.r(), 2);
            draw_number(renderer_, x + 5, y + 25, color.g(), 2);
            draw_number(renderer_, x + 5, y + 40, color.b(), 2);
        }

        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 150);
        SDL_RenderDrawPoint(renderer_, x - 2, y - 2);
    }

    static void render_rfid(int module_x, int module_y, int rfid_state, settings::ModuleAngle rotation)
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
        case settings::ModuleAngle::NORMAL:
            rfid_rel_x = scale_within_module(90);
            rfid_rel_y = scale_within_module(142);
            break;
        case settings::ModuleAngle::ROTATED_90:
            rfid_rel_x = scale_within_module(37);
            rfid_rel_y = scale_within_module(90);
            break;
        case settings::ModuleAngle::UPSIDE_DOWN:
            rfid_rel_x = scale_within_module(90);
            rfid_rel_y = scale_within_module(37);
            break;
        case settings::ModuleAngle::ROTATED_270:
            rfid_rel_x = scale_within_module(142);
            rfid_rel_y = scale_within_module(90);
            break;
        }

        SDL_Rect rfid_rect = {module_x + rfid_rel_x - 10, module_y + rfid_rel_y - 10, 20, 20};
        SDL_SetRenderDrawColor(renderer_, 255, 0, 0, 255);
        SDL_RenderFillRect(renderer_, &rfid_rect);
    }

    static void render_big_belt(int module_x, int module_y, module::BeltSpeed speed, bool is_running, settings::ModuleAngle rotation)
    {
        int width, height, belt_x, belt_y;

        switch (rotation)
        {
        case settings::ModuleAngle::NORMAL:
        case settings::ModuleAngle::UPSIDE_DOWN:
            width = scale_within_module(180);
            height = scale_within_module(30);
            belt_x = module_x;
            belt_y = module_y + scale_within_module(75);
            break;
        case settings::ModuleAngle::ROTATED_90:
        case settings::ModuleAngle::ROTATED_270:
            width = scale_within_module(30);
            height = scale_within_module(180);
            belt_x = module_x + scale_within_module(75);
            belt_y = module_y;
            break;
        };

        SDL_Rect belt = {belt_x, belt_y, width, height};

        if (is_running)
        {
            SDL_SetRenderDrawColor(renderer_, 34, 139, 34, 255);
        }
        else
        {
            SDL_SetRenderDrawColor(renderer_, 40, 40, 40, 255);
        }
        SDL_RenderFillRect(renderer_, &belt);

        if (is_running)
        {
            SDL_SetRenderDrawColor(renderer_, 50, 205, 50, 255);
        }
        else
        {
            SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
        }
        SDL_RenderDrawRect(renderer_, &belt);

        if (is_running && speed.raw() != 0)
        {
            if (settings::get_setting<bool>("debug"))
                draw_number(renderer_, belt_x, belt_y, speed.raw(), 2);

            SDL_SetRenderDrawColor(renderer_, 144, 238, 144, 150);
            double step = 20.0;
            double offset = fmod(belt_offset_ * (-speed.raw() / 100.0), step);
            int longer_dimension = height > width ? height : width;
            for (double l = -step; l < longer_dimension + step; l += step)
            {
                switch (rotation)
                {
                case settings::ModuleAngle::NORMAL:
                case settings::ModuleAngle::UPSIDE_DOWN:
                {
                    int draw_x = belt_x + (rotation == settings::ModuleAngle::NORMAL ? static_cast<int>(l + offset)
                                                                                     : width - static_cast<int>(l + offset));
                    if (draw_x >= belt_x && draw_x < belt_x + width)
                    {
                        SDL_RenderDrawLine(renderer_, draw_x, belt_y, draw_x, belt_y + height - 1);
                    };
                    break;
                }
                case settings::ModuleAngle::ROTATED_90:
                case settings::ModuleAngle::ROTATED_270:
                {
                    int draw_y = belt_y + (rotation == settings::ModuleAngle::ROTATED_90 ? static_cast<int>(l + offset)
                                                                                         : height - static_cast<int>(l + offset));
                    if (draw_y >= belt_y && draw_y < belt_y + height)
                    {
                        SDL_RenderDrawLine(renderer_, belt_x, draw_y, belt_x + width - 1, draw_y);
                    };
                    break;
                }
                }
            }
        }
    }

    static void render_small_belt(int module_x, int module_y, module::BeltSpeed speed, bool is_running, settings::ModuleAngle rotation)
    {
        int width, height, belt_x, belt_y;

        switch (rotation)
        {
        case settings::ModuleAngle::NORMAL:
            belt_x = module_x + scale_within_module(75);
            belt_y = module_y + scale_within_module(105);
            width = scale_within_module(30);
            height = scale_within_module(75);
            break;
        case settings::ModuleAngle::ROTATED_90:
            belt_x = module_x;
            belt_y = module_y + scale_within_module(75);
            width = scale_within_module(75);
            height = scale_within_module(30);
            break;
        case settings::ModuleAngle::UPSIDE_DOWN:
            belt_x = module_x + scale_within_module(75);
            belt_y = module_y;
            width = scale_within_module(30);
            height = scale_within_module(75);
            break;
        case settings::ModuleAngle::ROTATED_270:
            belt_x = module_x + scale_within_module(105);
            belt_y = module_y + scale_within_module(75);
            width = scale_within_module(75);
            height = scale_within_module(30);
            break;
        }

        SDL_Rect belt = {belt_x, belt_y, width, height};

        if (is_running)
        {
            SDL_SetRenderDrawColor(renderer_, 34, 139, 34, 255);
        }
        else
        {
            SDL_SetRenderDrawColor(renderer_, 40, 40, 40, 255);
        }
        SDL_RenderFillRect(renderer_, &belt);

        if (is_running)
        {
            SDL_SetRenderDrawColor(renderer_, 50, 205, 50, 255);
        }
        else
        {
            SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
        }
        SDL_RenderDrawRect(renderer_, &belt);

        // Animation lines
        if (is_running && speed.raw() != 0)
        {
            if (settings::get_setting<bool>("debug"))
                draw_number(renderer_, belt_x, belt_y, speed.raw(), 2);

            SDL_SetRenderDrawColor(renderer_, 144, 238, 144, 150);
            double step = 20.0;
            double offset = -fmod(belt_offset_ * (-speed.raw() / 100.0), step);
            int longer_dimension = height > width ? height : width;
            for (double l = -step; l < longer_dimension + step; l += step)
            {
                switch (rotation)
                {
                case settings::ModuleAngle::NORMAL:
                case settings::ModuleAngle::UPSIDE_DOWN:
                {
                    int draw_y = belt_y + (rotation == settings::ModuleAngle::NORMAL ? static_cast<int>(l + offset)
                                                                                     : height - static_cast<int>(l + offset));
                    if (draw_y >= belt_y && draw_y < belt_y + height)
                    {
                        SDL_RenderDrawLine(renderer_, belt_x, draw_y, belt_x + width - 1, draw_y);
                    }
                    break;
                }
                case settings::ModuleAngle::ROTATED_90:
                case settings::ModuleAngle::ROTATED_270:
                {
                    int draw_x = belt_x + (rotation == settings::ModuleAngle::ROTATED_270 ? static_cast<int>(l + offset)
                                                                                          : width - static_cast<int>(l + offset));
                    if (draw_x >= belt_x && draw_x < belt_x + width)
                    {
                        SDL_RenderDrawLine(renderer_, draw_x, belt_y, draw_x, belt_y + height - 1);
                    }
                    break;
                }
                }
            }
        }
    }

    static void render_laser(
        int module_x, int module_y, bool power, int detection, bool is_left, settings::ModuleAngle rotation)
    {
        if (power == false)
            return;

        // Blinking logic if detecting: 100ms ON, 100ms OFF
        if (detection != 0)
        {
            if ((SDL_GetTicks() / 100) % 2 == 0)
                return;
        }

        int x1, y1, x2, y2;
        int margin = scale_within_module(36); // 20% of 180

        switch (rotation)
        {
        case settings::ModuleAngle::NORMAL:
            x1 = x2 = module_x + (is_left ? margin : scale_within_module(180) - margin);
            y1 = module_y + scale_within_module(75);
            y2 = module_y + scale_within_module(105);
            break;
        case settings::ModuleAngle::ROTATED_90:
            x1 = module_x + scale_within_module(75);
            x2 = module_x + scale_within_module(105);
            y1 = y2 = module_y + (is_left ? margin : scale_within_module(180) - margin);
            break;
        case settings::ModuleAngle::UPSIDE_DOWN:
            x1 = x2 = module_x + (is_left ? scale_within_module(180) - margin : margin);
            y1 = module_y + scale_within_module(75);
            y2 = module_y + scale_within_module(105);
            break;
        case settings::ModuleAngle::ROTATED_270:
            x1 = module_x + scale_within_module(75);
            x2 = module_x + scale_within_module(105);
            y1 = y2 = module_y + (is_left ? scale_within_module(180) - margin : margin);
            break;
        }

        SDL_SetRenderDrawColor(renderer_, 255, 0, 0, 255);
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

        SDL_SetRenderDrawColor(renderer_, 100, 100, 100, 255);
        SDL_Rect dot1 = {x1 - 2, y1 - 2, 4, 4};
        SDL_Rect dot2 = {x2 - 2, y2 - 2, 4, 4};
        SDL_RenderFillRect(renderer_, &dot1);
        SDL_RenderFillRect(renderer_, &dot2);
    }

    static void render_servo(int module_x, int module_y, module::ServoAngle angle, settings::ModuleAngle rotation)
    {
        // Pivot at (90, 75) relative to module (top-middle of big belt)
        double pivot_rel_x = scale_within_module(90.0);
        double pivot_rel_y = scale_within_module(75.0);

        // Base angle depends on rotation
        double base_angle_rad = 0.0;
        switch (rotation)
        {
        case settings::ModuleAngle::NORMAL:
            base_angle_rad = 0.0;
            break;
        case settings::ModuleAngle::ROTATED_90:
            base_angle_rad = M_PI / 2.0;
            pivot_rel_x = scale_within_module(105.0);
            pivot_rel_y = scale_within_module(90.0);
            break;
        case settings::ModuleAngle::UPSIDE_DOWN:
            base_angle_rad = M_PI;
            pivot_rel_x = scale_within_module(90.0);
            pivot_rel_y = scale_within_module(105.0);
            break;
        case settings::ModuleAngle::ROTATED_270:
            base_angle_rad = 3.0 * M_PI / 2.0;
            pivot_rel_x = scale_within_module(75.0);
            pivot_rel_y = scale_within_module(90.0);
            break;
        }

        // angle 0: parallel to big belt (right), 90: perpendicular (towards small belt)
        double servo_angle_rad = (angle.raw() / 180.0) * M_PI;
        double total_angle_rad = base_angle_rad + servo_angle_rad;

        double length = scale_within_module(35.0);
        double end_rel_x = pivot_rel_x + length * cos(total_angle_rad);
        double end_rel_y = pivot_rel_y + length * sin(total_angle_rad);

        SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
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

        SDL_SetRenderDrawColor(renderer_, 50, 50, 50, 255);
        SDL_Rect pivot_rect = {x1 - 4, y1 - 4, 8, 8};
        SDL_RenderFillRect(renderer_, &pivot_rect);
        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        if (settings::get_setting<bool>("debug"))
            draw_number(renderer_, x1 - 12, y1 - 8, angle.raw(), 2);
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
            if (tub->module_location() == ModuleLocation::LEFT)
            {
                switch (module_settings.angle)
                {
                case settings::ModuleAngle::NORMAL:
                    rel_x = (1.0 - tub->module_location_progress()) * scale_within_module(90.0);
                    rel_y = scale_within_module(90.0);
                    break;
                case settings::ModuleAngle::ROTATED_90:
                    rel_x = scale_within_module(90.0);
                    rel_y = (1.0 - tub->module_location_progress()) * scale_within_module(90.0);
                    break;
                case settings::ModuleAngle::UPSIDE_DOWN:
                    rel_x = scale_within_module(90) + tub->module_location_progress() * scale_within_module(90.0);
                    rel_y = scale_within_module(90.0);
                    break;
                case settings::ModuleAngle::ROTATED_270:
                    rel_x = scale_within_module(90.0);
                    rel_y = scale_within_module(90) + tub->module_location_progress() * scale_within_module(90.0);
                    break;

                default:
                    throw std::invalid_argument("Invalid module angle " + std::to_string(static_cast<std::underlying_type<settings::ModuleAngle>::type>(module_settings.angle)));
                }
            }
            else if (tub->module_location() == ModuleLocation::RIGHT)
            {
                switch (module_settings.angle)
                {
                case settings::ModuleAngle::NORMAL:
                    rel_x = scale_within_module(90) + tub->module_location_progress() * scale_within_module(90.0);
                    rel_y = scale_within_module(90.0);
                    break;
                case settings::ModuleAngle::ROTATED_90:
                    rel_x = scale_within_module(90.0);
                    rel_y = scale_within_module(90) + tub->module_location_progress() * scale_within_module(90.0);
                    break;
                case settings::ModuleAngle::UPSIDE_DOWN:
                    rel_x = (1.0 - tub->module_location_progress()) * scale_within_module(90.0);
                    rel_y = scale_within_module(90.0);
                    break;
                case settings::ModuleAngle::ROTATED_270:
                    rel_x = scale_within_module(90.0);
                    rel_y = (1.0 - tub->module_location_progress()) * scale_within_module(90.0);
                    break;

                default:
                    throw std::invalid_argument("Invalid module angle " + std::to_string(static_cast<std::underlying_type<settings::ModuleAngle>::type>(module_settings.angle)));
                }
            }
            else
            {
                switch (module_settings.angle)
                {
                case settings::ModuleAngle::NORMAL:
                    rel_x = scale_within_module(90.0);
                    rel_y = scale_within_module(105.0) + tub->module_location_progress() * scale_within_module(75.0);
                    break;
                case settings::ModuleAngle::ROTATED_90:
                    rel_x = scale_within_module(75.0) - tub->module_location_progress() * scale_within_module(75.0);
                    rel_y = scale_within_module(90.0);
                    break;
                case settings::ModuleAngle::UPSIDE_DOWN:
                    rel_x = scale_within_module(90.0);
                    rel_y = scale_within_module(75.0) - tub->module_location_progress() * scale_within_module(75.0);
                    break;
                case settings::ModuleAngle::ROTATED_270:
                    rel_x = scale_within_module(105.0) + tub->module_location_progress() * scale_within_module(75.0);
                    rel_y = scale_within_module(90.0);
                    break;

                default:
                    throw std::invalid_argument("Invalid module angle " + std::to_string(static_cast<std::underlying_type<settings::ModuleAngle>::type>(module_settings.angle)));
                }
            }

            int x = module_x + (int)rel_x;
            int y = module_y + (int)rel_y;

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

            SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
            draw_number(renderer_, x - 4, y - 6, tub->id(), 2);
            if (settings::get_setting<bool>("debug"))
                draw_number(renderer_, x - 4, y + 5, tub->module_location_progress(), 1);
        }
    }

    static void render_module(module::ModuleId module_id, int x, int y)
    {
        const std::optional<std::shared_ptr<module::Module>> module_opt = module::get_module(module_id);
        if (!module_opt)
        {
            return;
        }

        const std::shared_ptr<module::Module> module = module_opt.value();
        const settings::ModuleSettings module_settings = settings::get_module_settings(module_id);

        SDL_Rect bg = {x, y, scale_within_module(180), scale_within_module(180)};
        SDL_SetRenderDrawColor(renderer_, 60, 60, 60, 255);
        SDL_RenderFillRect(renderer_, &bg);

        SDL_SetRenderDrawColor(renderer_, 200, 200, 200, 255);
        SDL_RenderDrawRect(renderer_, &bg);

        SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
        draw_number(renderer_, x + 5, y + 5, module_id.raw(), 2);

        render_big_belt(x, y, module->big_belt_speed(), module->big_belt_speed().raw() != 0, module_settings.angle);
        render_small_belt(x, y, module->small_belt_speed(), module->small_belt_speed().raw() != 0, module_settings.angle);
        render_led(x, y, module->led_color(), module_settings.angle);
        render_rfid(x, y, module->is_rfid_active(), module_settings.angle);
        render_servo(x, y, module->servo_angle(), module_settings.angle);
        render_laser(x, y, module->is_left_laser_active(), true, true, module_settings.angle);
        render_laser(x, y, module->is_right_laser_active(), true, false, module_settings.angle);
    }

    void stop()
    {
        running_ = false;

        SDL_DestroyRenderer(renderer_);
        SDL_DestroyWindow(window_);
        SDL_Quit();
    }

    static std::vector<std::unique_ptr<buttons::Button>> buttons_;

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
                                   800,
                                   800,
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
            stop();
            return;
        }

        buttons_.push_back(std::make_unique<buttons::PauseButton>(buttons::PauseButton()));
        buttons_.push_back(std::make_unique<buttons::SlowerButton>(buttons::SlowerButton()));
        buttons_.push_back(std::make_unique<buttons::SlowSlowerButton>(buttons::SlowSlowerButton()));
        buttons_.push_back(std::make_unique<buttons::FasterButton>(buttons::FasterButton()));
        buttons_.push_back(std::make_unique<buttons::FastFasterButton>(buttons::FastFasterButton()));
        buttons_.push_back(std::make_unique<buttons::SpeedButton>(buttons::SpeedButton()));
        buttons_.push_back(std::make_unique<buttons::TimeButton>(buttons::TimeButton()));
        buttons_.push_back(std::make_unique<buttons::ZoomInButton>(buttons::ZoomInButton()));
        buttons_.push_back(std::make_unique<buttons::ZoomOutButton>(buttons::ZoomOutButton()));
        buttons_.push_back(std::make_unique<buttons::ZoomButton>(buttons::ZoomButton()));
        buttons_.push_back(std::make_unique<buttons::DbgButton>(buttons::DbgButton()));
    }

    static std::chrono::milliseconds last_time = std::chrono::milliseconds(0);

    void update(virtual_clock::ISimulationClock &vclock)
    {
        if (!running_)
        {
            return;
        }

        scale = settings::get_setting<double>("scale");

        SDL_Event event;
        while (SDL_PollEvent(&event) != 0)
        {
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE)
            {
                stop();
                return;
            }

            if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
            {
                const uint8_t *keyboard = SDL_GetKeyboardState(NULL);

                if (keyboard[SDL_SCANCODE_F4])
                {
                    settings::set_setting("debug", !settings::get_setting<bool>("debug"));
                }
                if (keyboard[SDL_SCANCODE_UP])
                {
                    double new_value = settings::get_setting<double>("simulation_speed") + 0.1;
                    settings::set_setting("simulation_speed", new_value);
                    vclock.set_simulation_speed(new_value);
                }
                if (keyboard[SDL_SCANCODE_DOWN])
                {
                    double new_value = fmax(0.0, settings::get_setting<double>("simulation_speed") - 0.1);
                    settings::set_setting("simulation_speed", new_value);
                    vclock.set_simulation_speed(new_value);
                }
                if (keyboard[SDL_SCANCODE_LCTRL] && keyboard[SDL_SCANCODE_MINUS])
                {
                    double new_value = fmax(0.0, settings::get_setting<double>("scale") - 0.1);
                    settings::set_setting("scale", new_value);
                }
                if (keyboard[SDL_SCANCODE_LCTRL] && keyboard[SDL_SCANCODE_EQUALS])
                {
                    double new_value = fmin(5.0, settings::get_setting<double>("scale") + 0.1);
                    settings::set_setting("scale", new_value);
                }
            }

            for (auto &button : buttons_)
            {
                button->handle(event, vclock);
            }
        }

        if (last_time.count() == 0)
        {
            last_time = vclock.get_virtual_time();
        }

        const std::chrono::milliseconds current_time = vclock.get_virtual_time();
        const std::chrono::milliseconds dt = current_time - last_time;
        last_time = current_time;

        SDL_SetRenderDrawColor(renderer_, 30, 30, 30, 255);
        SDL_RenderClear(renderer_);

        for (int i = 1; i < MAX_MODULES; ++i)
        {
            const auto mid = module::ModuleId(i);

            if (!settings::get_module_settings(mid).active)
                continue;

            int x, y;
            get_module_coords(mid, x, y);
            render_module(mid, x, y);
        }

        render_tubs();
        for (auto &button : buttons_)
        {
            button->render(renderer_, vclock);
        }
        belt_offset_ += (dt.count() / 16.0) * BELT_FACTOR;

        SDL_RenderPresent(renderer_);
    }

    bool is_running()
    {
        return running_;
    }
}