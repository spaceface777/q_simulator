#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace firmware::crash {
	enum class Cause : uint8_t { FAULT };
	inline constexpr std::size_t MAX_FRAMES = 64;

	struct Capture {
		Cause                             cause   = Cause::FAULT;
		int                               signal  = 0;
		uintptr_t                         address = 0;
		std::array<uintptr_t, MAX_FRAMES> frames{};
		std::size_t                       frame_count = 0;
		std::size_t                       omitted     = 0;
		std::size_t                       omitted_at  = 0;
	};

	// Only firmware runs inside this guard. A recovered image must never execute again.
	[[nodiscard]] bool        run_guarded(void (*call)(void*), void* argument, Capture& capture);
	[[nodiscard]] std::string summary(const Capture& capture);
	[[nodiscard]] std::string report(const Capture& capture, unsigned module, uint64_t tick, uint64_t module_time_us);
} // namespace firmware::crash
