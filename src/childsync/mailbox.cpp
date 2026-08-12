#include "mailbox.h"

namespace childsync {

	MailboxStatus mailbox_append(Mailbox& mailbox, const uint8_t* bytes, std::size_t length) noexcept {
		switch (quercus_mailbox_append(&mailbox, bytes, static_cast<unsigned>(length))) {
		case 0:      return MailboxStatus::OK;
		case -EBUSY: return MailboxStatus::BUSY;
		default:     return MailboxStatus::NO_MEMORY;
		}
	}

	const uint8_t* mailbox_read(const Mailbox& mailbox, std::size_t index, std::size_t& length) noexcept {
		unsigned       found = 0;
		const uint8_t* bytes = quercus_mailbox_read(&mailbox, static_cast<unsigned>(index), &found);
		length               = found;
		return bytes;
	}

	bool mailbox_valid(const Mailbox& mailbox) noexcept {
		return quercus_mailbox_valid(&mailbox) != 0;
	}

} // namespace childsync
