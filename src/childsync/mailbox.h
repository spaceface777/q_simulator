#ifndef CHILDSYNC_MAILBOX_H
#define CHILDSYNC_MAILBOX_H

#include <cstddef>
#include <cstdint>

#include <quercus/lockstep_shm.h>

#include "../defs.h"

namespace childsync {

	// The layout comes from the SDK header the applications compile against, so the two sides
	// cannot disagree about it. The barrier alternates strictly: the parent writes only while
	// children are blocked and each child writes only while the parent is blocked, so the
	// mailboxes need no internal atomics.

	using Mailbox = quercus_mailbox_t;
	using Segment = quercus_segment_t;
	using Control = quercus_control_t;

	inline constexpr std::size_t MAILBOX_SLOTS = QUERCUS_MAILBOX_SLOTS;

	inline constexpr std::size_t MAILBOX_CAPACITY = QUERCUS_MAILBOX_CAPACITY;
	inline constexpr std::size_t MAX_BATCH_TICKS  = QUERCUS_BATCH_MAX_TICKS;

	static_assert(sizeof(Mailbox) == QUERCUS_MAILBOX_PAGE, "A mailbox must be exactly one page");
	static_assert(MAILBOX_CAPACITY <= UINT16_MAX, "Offsets must fit in uint16_t");
	static_assert(MODULE_TABLE_SIZE == QUERCUS_MODULE_TABLE_SIZE, "The module tables must be the same size");

	// Frames the platform may deliver to one application per tick, before the byte budget applies.
	inline constexpr std::size_t MAX_FRAMES_PER_TICK = MAILBOX_SLOTS;

	[[nodiscard]] inline std::size_t mailbox_count(const Mailbox& mailbox) noexcept {
		return quercus_mailbox_count(&mailbox);
	}

	[[nodiscard]] inline std::size_t mailbox_used(const Mailbox& mailbox) noexcept {
		return quercus_mailbox_used(&mailbox);
	}

	inline void mailbox_clear(Mailbox& mailbox) noexcept {
		quercus_mailbox_clear(&mailbox);
	}

	enum class MailboxStatus {
		OK,
		BUSY,      // All four slots are taken; maps to -EBUSY for the application.
		NO_MEMORY, // The bytes do not fit; maps to -ENOMEM for the application.
	};

	// Appends length bytes as one message. The mailbox is unchanged unless the result is OK.
	[[nodiscard]] MailboxStatus mailbox_append(Mailbox& mailbox, const uint8_t* bytes, std::size_t length) noexcept;

	// Returns the bytes of message index, or nullptr with length zero when the slot is unused.
	[[nodiscard]] const uint8_t* mailbox_read(const Mailbox& mailbox, std::size_t index, std::size_t& length) noexcept;

	// A child shares the page, so it can write any offsets at all. The parent must check them
	// before reading rather than trusting the child not to run it off the end of the page.
	[[nodiscard]] bool mailbox_valid(const Mailbox& mailbox) noexcept;

} // namespace childsync

#endif // CHILDSYNC_MAILBOX_H
