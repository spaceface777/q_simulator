#pragma once

#include "crash.h"

#include <cstdint>

namespace firmware::crash {

	// Coroutine and thread stacks are 8 MiB. A frame record further than this from the stack
	// pointer is not part of the chain.
	inline constexpr uintptr_t STACK_SPAN = uintptr_t{64} << 20;

	// Follows saved frame-pointer records, {previous frame pointer, return address}, appending return
	// addresses to the capture. Each record must lie above the previous one on the same stack.
	// A deep chain, such as runaway recursion, keeps its first and its last frames: where it failed
	// and where it began. Async-signal-safe if `read` is.
	template <typename Read, typename Strip>
	void walk_frames(Capture& capture, uintptr_t frame, uintptr_t low, Read read, Strip strip) noexcept {
		constexpr std::size_t       TAIL = 16;
		const std::size_t           head = MAX_FRAMES - TAIL;
		std::array<uintptr_t, TAIL> tail{};
		std::size_t                 seen = 0; // Return addresses found after the head was full
		while (frame != 0) {
			if (frame % sizeof(uintptr_t) != 0 || frame < low || frame - low > STACK_SPAN) break;
			uintptr_t record[2];
			if (!read(frame, record, sizeof(record))) break;
			const uintptr_t return_address = strip(record[1]);
			if (return_address == 0) break;
			if (capture.frame_count < head) capture.frames[capture.frame_count++] = return_address;
			else tail[seen++ % TAIL] = return_address;
			if (record[0] <= frame) break;
			low   = frame;
			frame = record[0];
		}
		const std::size_t kept = seen < TAIL ? seen : TAIL;
		capture.omitted_at     = capture.frame_count;
		capture.omitted        = seen - kept;
		for (std::size_t index = seen - kept; index < seen; index++)
			capture.frames[capture.frame_count++] = tail[index % TAIL];
	}

} // namespace firmware::crash
