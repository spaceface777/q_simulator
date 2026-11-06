#pragma once

#include <cstdint>

// The watchdog shared by the platform guards. It times guarded calls and asks the platform to
// interrupt one that runs past the limit. Internal to crash_posix.cpp and crash_windows.cpp.
namespace firmware::crash::watchdog {

	// Starts the watchdog thread once. `interrupt` runs on that thread for a call past the limit.
	void start(void (*interrupt)(uint64_t call));

	// A new nonzero number for a guarded call.
	[[nodiscard]] uint64_t next_call() noexcept;

	// Which call is running now, or 0 for none.
	void                   watch(uint64_t call) noexcept;
	[[nodiscard]] uint64_t watched() noexcept;

} // namespace firmware::crash::watchdog
