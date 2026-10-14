#ifndef SIM_MODULE_CLOCK_H
#define SIM_MODULE_CLOCK_H

#include <chrono>
#include <cstdint>

namespace sim {

	inline constexpr std::chrono::microseconds TICK_DURATION{1000};

	struct ModuleClockTick {
		bool     eligible = false;
		bool     skipped  = false;
		bool     granted  = false;
		uint32_t delta_us = 0;

		bool operator==(const ModuleClockTick&) const = default;
	};

	class ModuleClock {
	private:
		uint64_t start_tick_     = 0;
		int64_t  rate_ppm_       = 1'000'000;
		int64_t  remainder_      = 0;
		uint32_t skip_ppm_       = 0;
		uint64_t skip_rng_state_ = 0;

		static uint64_t next_random(uint64_t& state) noexcept;

	public:
		ModuleClock() = default;
		ModuleClock(uint64_t start_tick, int64_t rate_ppm, uint32_t skip_ppm, uint64_t skip_rng_state) noexcept;

		// Advances the clock state for one global tick. Ineligible and skipped ticks do not advance
		// native time. Copy this object to plan transitions without committing them.
		[[nodiscard]] ModuleClockTick advance(uint64_t global_tick, bool active, bool paused) noexcept;

		[[nodiscard]] uint64_t start_tick() const noexcept { return start_tick_; }

		[[nodiscard]] int64_t rate_ppm() const noexcept { return rate_ppm_; }

		[[nodiscard]] uint32_t skip_ppm() const noexcept { return skip_ppm_; }
	};

} // namespace sim

#endif // SIM_MODULE_CLOCK_H
