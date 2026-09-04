#ifndef SIM_RUNAHEAD_H
#define SIM_RUNAHEAD_H

#include <cstdint>
#include <limits>
#include <span>

namespace sim::runahead {

	// Maps one child-local event certificate through the exact future global grant sequence. The
	// returned count contains only slots whose cumulative local time remains strictly before the
	// event; a native_sim event at the transport timestamp runs before the priority-999 barrier.
	inline uint32_t safe_prefix(uint64_t now_us, uint64_t next_event_us, std::span<const bool> granted,
								uint32_t max_ticks, std::span<const uint32_t> delta_us) {
		if (next_event_us <= now_us) return 0;
		if (next_event_us == std::numeric_limits<uint64_t>::max()) return max_ticks;

		const uint64_t until_event = next_event_us - now_us;
		uint64_t       elapsed     = 0;
		for (uint32_t index = 0; index < max_ticks; index++) {
			if (!granted[index]) continue;
			elapsed += delta_us[index];
			if (elapsed >= until_event) return index;
		}
		return max_ticks;
	}

} // namespace sim::runahead

#endif // SIM_RUNAHEAD_H
