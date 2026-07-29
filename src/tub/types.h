#ifndef TUB_TYPES_H
#define TUB_TYPES_H

#include <algorithm>
#include <stdexcept>
#include <stdint.h>

#include "../defs.h"
#include "../util.h"

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
        util::require_range(id > 0, "Tub id out of range");

        value_ = id;
    }

    [[nodiscard]] constexpr uint8_t raw() const noexcept { return value_; }

    constexpr bool operator==(const TubId &other) const noexcept { return value_ == other.value_; }

    constexpr bool operator==(const uint8_t &other) const noexcept { return value_ == other; }
};

class LocationProgress
{
  private:
    static constexpr double EXTREMES_EPSILON = 0.1;

    double value_ = 0;

  public:
    constexpr LocationProgress() noexcept = default;

    explicit constexpr LocationProgress(double progress)
    {
        util::require_range(progress >= 0 - EXTREMES_EPSILON && progress <= 1.0 + EXTREMES_EPSILON,
                            "Location progress must be between 0 and 1.0");

        value_ = progress;
    }

    [[nodiscard]] constexpr bool is_at_end() const noexcept { return value_ > 1; }

    [[nodiscard]] constexpr bool is_at_begin() const noexcept { return value_ < 0; }

    [[nodiscard]] constexpr double raw() const noexcept { return value_; }

    constexpr bool operator==(const LocationProgress &other) const noexcept { return value_ == other.value_; }

    constexpr bool operator<(const double &other) const noexcept { return value_ < other; }

    constexpr bool operator>(const double &other) const noexcept { return value_ > other; }

    constexpr LocationProgress &operator+=(double delta)
    {
        *this = LocationProgress(std::clamp(value_ + delta, 0.0 - EXTREMES_EPSILON, 1.0 + EXTREMES_EPSILON));
        return *this;
    }
};

} // namespace tub

#endif