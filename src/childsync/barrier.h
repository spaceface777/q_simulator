#ifndef CHILDSYNC_BARRIER_H
#define CHILDSYNC_BARRIER_H

#include <chrono>
#include <cstdint>
#include <string>

#include "mailbox.h"

namespace childsync {

	// Both sides use the GCC atomic builtins rather than std::atomic, so the C++ parent and the C
	// child adapter agree on the memory model without either side casting the shared page.

	inline void store_release(uint32_t& word, uint32_t value) noexcept {
		__atomic_store_n(&word, value, __ATOMIC_RELEASE);
	}

	[[nodiscard]] inline uint32_t load_acquire(const uint32_t& word) noexcept {
		return __atomic_load_n(&word, __ATOMIC_ACQUIRE);
	}

	inline void store_relaxed(uint32_t& word, uint32_t value) noexcept {
		__atomic_store_n(&word, value, __ATOMIC_RELAXED);
	}

	// Returns the value held before the increment, so the caller can tell whether it was the last.
	// Acquire-release: the last one in acquires what every other participant released.
	[[nodiscard]] inline uint32_t fetch_add_release(uint32_t& word, uint32_t value) noexcept {
		return __atomic_fetch_add(&word, value, __ATOMIC_ACQ_REL);
	}

	// Wakes up to count waiters. Returns how many were woken, or -1 on failure.
	int futex_wake(uint32_t& word, int count) noexcept;

	// Bitset futex operations use an absolute CLOCK_MONOTONIC timeout. The bit mask is notification
	// routing only; callers must always use a separate state word as their progress predicate.
	int futex_wake_bitset(uint32_t& word, int count, uint32_t mask) noexcept;

	bool futex_wait_bitset(uint32_t& word, uint32_t expected, uint32_t mask,
						   std::chrono::microseconds timeout) noexcept;

	// Returns false if the wait timed out. A mismatch against expected returns true immediately,
	// which is the caller's cue to re-read the word rather than to assume progress.
	bool futex_wait(uint32_t& word, uint32_t expected, std::chrono::microseconds timeout) noexcept;

	// A shared mapping backed by an anonymous file, handed to children as an inherited descriptor.
	// The kernel frees it once the parent and every child have exited, so there is nothing to unlink.
	class Mapping {
	private:
		int    fd_    = -1;
		void*  base_  = nullptr;
		size_t bytes_ = 0;

		void reset() noexcept;

	public:
		Mapping() = default;

		// Creates and zeroes a new mapping. Throws on failure.
		Mapping(const char* name, std::size_t bytes);

		// Maps a descriptor inherited across exec. Throws on failure.
		Mapping(int fd, std::size_t bytes);

		~Mapping();

		Mapping(const Mapping&)            = delete;
		Mapping& operator=(const Mapping&) = delete;

		Mapping(Mapping&& other) noexcept;
		Mapping& operator=(Mapping&& other) noexcept;

		[[nodiscard]] bool is_valid() const noexcept { return base_ != nullptr; }

		[[nodiscard]] int fd() const noexcept { return fd_; }

		[[nodiscard]] void* base() const noexcept { return base_; }

		// Clears FD_CLOEXEC so the descriptor survives execl(). Throws on failure.
		void keep_across_exec() const;
	};

	// The barrier words every participant shares.
	class SharedControl {
	private:
		Mapping mapping_;

	public:
		SharedControl() : mapping_("quercus-control", sizeof(Control)) {}

		explicit SharedControl(int fd) : mapping_(fd, sizeof(Control)) {}

		[[nodiscard]] Control& get() const noexcept { return *static_cast<Control*>(mapping_.base()); }

		[[nodiscard]] const Mapping& mapping() const noexcept { return mapping_; }
	};

	// One child's per-tick mailbox arrays. Kept out of the control mapping so no child can read another's.
	class ChildSegment {
	private:
		Mapping mapping_;

	public:
		ChildSegment() : mapping_("quercus-mailbox", sizeof(Segment)) {}

		explicit ChildSegment(int fd) : mapping_(fd, sizeof(Segment)) {}

		[[nodiscard]] Segment& get() const noexcept { return *static_cast<Segment*>(mapping_.base()); }

		[[nodiscard]] const Mapping& mapping() const noexcept { return mapping_; }
	};

} // namespace childsync

#endif // CHILDSYNC_BARRIER_H
