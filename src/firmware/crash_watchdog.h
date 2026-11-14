#pragma once

#include <cstdint>

// Times firmware grants and requests a signal when a grant exceeds its limit.
namespace firmware::crash::watchdog {

	// Starts the watchdog thread once. `interrupt` runs on that thread for a call past the limit.
	void start(void (*interrupt)(uint64_t call));

	// A new nonzero number for a guarded call.
	[[nodiscard]] uint64_t next_call() noexcept;

	// Which call is running now, or 0 for none.
	void                   watch(uint64_t call) noexcept;
	[[nodiscard]] uint64_t watched() noexcept;

} // namespace firmware::crash::watchdog
