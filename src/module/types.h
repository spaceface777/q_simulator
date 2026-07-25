#ifndef BELT_SPEED_H
#define BELT_SPEED_H

#include <stdint.h>
#include <stdexcept>
#include "../util.h"
#include "../defs.h"

namespace module
{
    class ModuleId
    {
    private:
        uint8_t value_ = 0;

    public:
        constexpr ModuleId() noexcept = default;

        explicit constexpr ModuleId(uint8_t id)
        {
            util::require_range(id > 0 && id < MAX_MODULES, "Module id out of range");

            value_ = id;
        }

        [[nodiscard]] constexpr uint8_t raw() const noexcept
        {
            return value_;
        }

        constexpr bool operator==(const ModuleId &other) const noexcept
        {
            return value_ == other.value_;
        }

        constexpr bool operator==(const uint8_t &other) const noexcept
        {
            return value_ == other;
        }
    };

    class ServoAngle
    {
    private:
        uint8_t value_ = 0;

    public:
        constexpr ServoAngle() noexcept = default;

        explicit constexpr ServoAngle(uint8_t angle)
        {
            util::require_range(angle > 0 && angle <= 180, "Servo angle must be between 0 and 180");

            value_ = angle;
        }

        [[nodiscard]] constexpr uint8_t raw() const noexcept
        {
            return value_;
        }
    };

    class BeltSpeed
    {
    private:
        int8_t value_ = 0;

    public:
        constexpr BeltSpeed() noexcept = default;

        explicit constexpr BeltSpeed(int8_t speed)
        {
            util::require_range(speed >= -100 && speed <= 100, "Belt speed must be between -100 and 100");

            value_ = speed;
        }

        [[nodiscard]] constexpr int8_t raw() const noexcept
        {
            return value_;
        }
    };

    class LedColor
    {
    private:
        uint32_t value_ = 0;
        uint8_t r_ = 0;
        uint8_t g_ = 0;
        uint8_t b_ = 0;
        uint8_t a_ = 0;

    public:
        constexpr LedColor() noexcept = default;

        explicit constexpr LedColor(uint32_t color)
        {
            r_ = color & 0xFF;
            g_ = (color >> 8) & 0xFF;
            b_ = (color >> 16) & 0xFF;
            a_ = (color >> 24) & 0xFF;

            value_ = color;
        }

        [[nodiscard]] constexpr uint32_t raw() const noexcept
        {
            return value_;
        }

        [[nodiscard]] constexpr uint8_t r() const noexcept
        {
            return r_;
        }

        [[nodiscard]] constexpr uint8_t g() const noexcept
        {
            return g_;
        }

        [[nodiscard]] constexpr uint8_t b() const noexcept
        {
            return b_;
        }

        [[nodiscard]] constexpr uint8_t a() const noexcept
        {
            return a_;
        }
    };
}

#endif