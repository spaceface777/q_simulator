#include "module_clock.h"

#include <algorithm>

namespace sim {

	ModuleClock::ModuleClock(uint64_t start_tick, int64_t rate_ppm, uint32_t skip_ppm, uint64_t skip_rng_state) noexcept
		: start_tick_(start_tick), rate_ppm_(rate_ppm), skip_ppm_(skip_ppm), skip_rng_state_(skip_rng_state) {}

	uint64_t ModuleClock::next_random(uint64_t& state) noexcept {
		state += 0x9E3779B97F4A7C15ULL;
		uint64_t value = state;
		value          = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
		value          = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
		return value ^ (value >> 31);
	}

	ModuleClockTick ModuleClock::advance(uint64_t global_tick, bool active, bool paused) noexcept {
		ModuleClockTick tick;
		tick.eligible = active && global_tick >= start_tick_ && !paused;
		if (!tick.eligible) return tick;

		tick.skipped = skip_ppm_ != 0 && (next_random(skip_rng_state_) % 1'000'000u) < skip_ppm_;
		if (tick.skipped) return tick;

		tick.granted = true;
		remainder_ += TICK_DURATION.count() * rate_ppm_;
		const int64_t delta = remainder_ / 1'000'000;
		remainder_ %= 1'000'000;
		tick.delta_us = static_cast<uint32_t>(std::max<int64_t>(1, delta));
		return tick;
	}

} // namespace sim
