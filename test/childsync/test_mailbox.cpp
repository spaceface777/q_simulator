#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "../../src/childsync/mailbox.h"

namespace {

	std::vector<uint8_t> pattern(std::size_t length, uint8_t first) {
		std::vector<uint8_t> bytes(length);
		for (std::size_t i = 0; i < length; i++) {
			bytes[i] = static_cast<uint8_t>(first + i);
		}
		return bytes;
	}

	childsync::MailboxStatus append(childsync::Mailbox& mailbox, const std::vector<uint8_t>& bytes) {
		return childsync::mailbox_append(mailbox, bytes.data(), bytes.size());
	}

	std::vector<uint8_t> read(const childsync::Mailbox& mailbox, std::size_t index) {
		std::size_t    length = 0;
		const uint8_t* bytes  = childsync::mailbox_read(mailbox, index, length);
		if (bytes == nullptr) {
			return {};
		}
		return std::vector<uint8_t>(bytes, bytes + length);
	}

	TEST(Mailbox, OccupiesExactlyOnePage) {
		EXPECT_EQ(sizeof(childsync::Mailbox), 4096u);
		EXPECT_EQ(childsync::MAILBOX_CAPACITY, 4088u);
	}

	TEST(Mailbox, StartsEmpty) {
		childsync::Mailbox mailbox{};
		EXPECT_EQ(childsync::mailbox_count(mailbox), 0u);
		EXPECT_EQ(childsync::mailbox_used(mailbox), 0u);
		EXPECT_TRUE(read(mailbox, 0).empty());
	}

	TEST(Mailbox, ReadsBackWhatWasAppended) {
		childsync::Mailbox         mailbox{};
		const std::vector<uint8_t> first  = pattern(10, 1);
		const std::vector<uint8_t> second = pattern(300, 40);

		ASSERT_EQ(append(mailbox, first), childsync::MailboxStatus::OK);
		ASSERT_EQ(append(mailbox, second), childsync::MailboxStatus::OK);

		EXPECT_EQ(childsync::mailbox_count(mailbox), 2u);
		EXPECT_EQ(childsync::mailbox_used(mailbox), 310u);
		EXPECT_EQ(read(mailbox, 0), first);
		EXPECT_EQ(read(mailbox, 1), second);
		EXPECT_TRUE(read(mailbox, 2).empty());
	}

	TEST(Mailbox, RefusesAFifthMessage) {
		childsync::Mailbox mailbox{};
		for (int i = 0; i < 4; i++) {
			ASSERT_EQ(append(mailbox, pattern(8, static_cast<uint8_t>(i))), childsync::MailboxStatus::OK);
		}

		EXPECT_EQ(append(mailbox, pattern(8, 9)), childsync::MailboxStatus::BUSY);
		EXPECT_EQ(childsync::mailbox_count(mailbox), 4u);
		EXPECT_EQ(childsync::mailbox_used(mailbox), 32u);
	}

	// The page holds 4088 bytes, so the slot count is not the only limit that bites.
	TEST(Mailbox, RefusesFourMaximalFrames) {
		childsync::Mailbox mailbox{};
		for (int i = 0; i < 3; i++) {
			ASSERT_EQ(append(mailbox, pattern(1024, static_cast<uint8_t>(i))), childsync::MailboxStatus::OK);
		}

		EXPECT_EQ(append(mailbox, pattern(1024, 3)), childsync::MailboxStatus::NO_MEMORY);

		// What is left still fits, so the refusal is about bytes rather than about the slot.
		EXPECT_EQ(append(mailbox, pattern(1016, 3)), childsync::MailboxStatus::OK);
		EXPECT_EQ(childsync::mailbox_used(mailbox), childsync::MAILBOX_CAPACITY);
	}

	TEST(Mailbox, LeavesTheMailboxUnchangedWhenItRefuses) {
		childsync::Mailbox         mailbox{};
		const std::vector<uint8_t> kept = pattern(16, 7);
		ASSERT_EQ(append(mailbox, kept), childsync::MailboxStatus::OK);

		EXPECT_EQ(append(mailbox, pattern(childsync::MAILBOX_CAPACITY, 0)), childsync::MailboxStatus::NO_MEMORY);

		EXPECT_EQ(childsync::mailbox_count(mailbox), 1u);
		EXPECT_EQ(read(mailbox, 0), kept);
	}

	// A zero-length message would leave an end offset that reads back as an unused slot.
	TEST(Mailbox, RefusesAnEmptyMessage) {
		childsync::Mailbox mailbox{};
		EXPECT_EQ(childsync::mailbox_append(mailbox, nullptr, 0), childsync::MailboxStatus::NO_MEMORY);
		EXPECT_EQ(childsync::mailbox_count(mailbox), 0u);
	}

	TEST(Mailbox, ClearMakesItReusable) {
		childsync::Mailbox mailbox{};
		ASSERT_EQ(append(mailbox, pattern(100, 1)), childsync::MailboxStatus::OK);

		childsync::mailbox_clear(mailbox);

		EXPECT_EQ(childsync::mailbox_count(mailbox), 0u);
		EXPECT_EQ(childsync::mailbox_used(mailbox), 0u);

		const std::vector<uint8_t> reused = pattern(5, 200);
		ASSERT_EQ(append(mailbox, reused), childsync::MailboxStatus::OK);
		EXPECT_EQ(read(mailbox, 0), reused);
	}

	TEST(Mailbox, AcceptsOffsetsItWroteItself) {
		childsync::Mailbox mailbox{};
		EXPECT_TRUE(childsync::mailbox_valid(mailbox));

		ASSERT_EQ(append(mailbox, pattern(10, 1)), childsync::MailboxStatus::OK);
		ASSERT_EQ(append(mailbox, pattern(10, 1)), childsync::MailboxStatus::OK);
		EXPECT_TRUE(childsync::mailbox_valid(mailbox));
	}

	// The child shares the page, so the parent has to assume the offsets may be anything at all.
	TEST(Mailbox, RejectsOffsetsAChildCouldCorrupt) {
		childsync::Mailbox mailbox{};

		mailbox.end[0] = static_cast<uint16_t>(childsync::MAILBOX_CAPACITY + 1);
		EXPECT_FALSE(childsync::mailbox_valid(mailbox));

		childsync::mailbox_clear(mailbox);
		mailbox.end[0] = 20;
		mailbox.end[1] = 10;
		EXPECT_FALSE(childsync::mailbox_valid(mailbox));

		childsync::mailbox_clear(mailbox);
		mailbox.end[0] = 20;
		mailbox.end[1] = 20;
		EXPECT_FALSE(childsync::mailbox_valid(mailbox));

		// A hole: slot 1 is unused but slot 2 claims bytes, which mailbox_count would never reach.
		childsync::mailbox_clear(mailbox);
		mailbox.end[0] = 20;
		mailbox.end[2] = 40;
		EXPECT_FALSE(childsync::mailbox_valid(mailbox));
	}

} // namespace
