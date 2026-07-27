#ifndef TUB_TYPES_H
#define TUB_TYPES_H

#include <stdint.h>
#include <stdexcept>
#include "../util.h"
#include "../defs.h"

namespace tub
{

    class TubId
    {
    private:
        uint8_t value_ = 0;

    public:
        constexpr TubId() noexcept = default;

        explicit constexpr TubId(uint8_t id)
        {
            value_ = id;
        }

        [[nodiscard]] constexpr uint8_t raw() const noexcept
        {
            return value_;
        }

        constexpr bool operator==(const TubId &other) const noexcept
        {
            return value_ == other.value_;
        }

        constexpr bool operator==(const uint8_t &other) const noexcept
        {
            return value_ == other;
        }
    };

    class LocationProgress
    {
    private:
        double value_ = 0;

    public:
        constexpr LocationProgress() noexcept = default;

        explicit constexpr LocationProgress(double progress)
        {
            util::require_range(progress >= 0 && progress <= 1.0, "Location progress must be between 0 and 1.0");

            value_ = progress;
        }

        [[nodiscard]] constexpr double raw() const noexcept
        {
            return value_;
        }

        constexpr bool operator<(const double &other) const noexcept
        {
            return value_ < other;
        }

        constexpr bool operator>(const double &other) const noexcept
        {
            return value_ > other;
        }

        constexpr LocationProgress &operator+=(double delta)
        {
            *this = LocationProgress(value_ + delta);
            return *this;
        }
    };

}

#endif