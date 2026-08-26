#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

#include "../../src/rpc/frame.h"

namespace {

	rpc::Frame make_request(uint8_t cmd, std::vector<uint8_t> payload) {
		rpc::Frame frame;
		frame.type    = MT_REQ;
		frame.cmd     = cmd;
		frame.call_id = 0x8042;
		frame.payload = std::move(payload);
		return frame;
	}

	void feed(rpc::FrameReader& reader, const std::vector<uint8_t>& bytes) {
		reader.append(bytes.data(), bytes.size());
	}

} // namespace

TEST(FrameTest, RoundTripsRequest) {
	rpc::Frame sent             = make_request(CMD_SERVO_ANGLE_SET, {1, 2, 3, 4});
	sent.observation_request_id = 42;
	sent.handler_work_id        = 43;
	sent.stop_after_delivery    = true;

	rpc::FrameReader reader;
	feed(reader, rpc::encode(sent));

	rpc::Frame received;
	ASSERT_EQ(reader.next(received), rpc::DecodeStatus::OK);
	EXPECT_EQ(received.type, MT_REQ);
	EXPECT_EQ(received.cmd, CMD_SERVO_ANGLE_SET);
	EXPECT_EQ(received.call_id, 0x8042);
	EXPECT_EQ(received.payload, sent.payload);
	EXPECT_EQ(received.observation_request_id, 0u);
	EXPECT_EQ(received.handler_work_id, 0u);
	EXPECT_FALSE(received.stop_after_delivery);
	EXPECT_EQ(reader.buffered(), 0u);
}

TEST(FrameTest, RoundTripsNegativeStatusOnResponse) {
	rpc::Frame sent;
	sent.type    = MT_RESP;
	sent.cmd     = CMD_BELT_BIG_SET_SPEED;
	sent.call_id = 7;
	sent.status  = -11; // -EAGAIN

	rpc::FrameReader reader;
	feed(reader, rpc::encode(sent));

	rpc::Frame received;
	ASSERT_EQ(reader.next(received), rpc::DecodeStatus::OK);
	EXPECT_EQ(received.status, -11);
	EXPECT_TRUE(received.payload.empty());
}

TEST(FrameTest, RoundTripsFlagsOnRequest) {
	rpc::Frame sent = make_request(CMD_LOG, {'h', 'i'});
	sent.flags      = RPC_FLAG_NON_CON;

	rpc::FrameReader reader;
	feed(reader, rpc::encode(sent));

	rpc::Frame received;
	ASSERT_EQ(reader.next(received), rpc::DecodeStatus::OK);
	EXPECT_EQ(received.flags, RPC_FLAG_NON_CON);
}

TEST(FrameTest, ReassemblesFrameDeliveredOneByteAtATime) {
	// The previous TOML transport silently truncated a message split across two reads.
	const std::vector<uint8_t> bytes = rpc::encode(make_request(CMD_RFID_GET_UID, {9, 8, 7, 6, 5}));

	rpc::FrameReader reader;
	rpc::Frame       received;

	for (std::size_t i = 0; i + 1 < bytes.size(); i++) {
		reader.append(&bytes[i], 1);
		ASSERT_EQ(reader.next(received), rpc::DecodeStatus::INCOMPLETE) << "at byte " << i;
	}

	reader.append(&bytes[bytes.size() - 1], 1);
	ASSERT_EQ(reader.next(received), rpc::DecodeStatus::OK);
	EXPECT_EQ(received.payload, std::vector<uint8_t>({9, 8, 7, 6, 5}));
}

TEST(FrameTest, YieldsEveryFrameFromACoalescedRead) {
	std::vector<uint8_t> bytes;
	for (uint8_t i = 0; i < 5; i++) {
		const std::vector<uint8_t> encoded = rpc::encode(make_request(CMD_HEARTBEAT, {i}));
		bytes.insert(bytes.end(), encoded.begin(), encoded.end());
	}

	rpc::FrameReader reader;
	feed(reader, bytes);

	rpc::Frame received;
	for (uint8_t i = 0; i < 5; i++) {
		ASSERT_EQ(reader.next(received), rpc::DecodeStatus::OK) << "frame " << static_cast<int>(i);
		ASSERT_EQ(received.payload.size(), 1u);
		EXPECT_EQ(received.payload[0], i);
	}
	EXPECT_EQ(reader.next(received), rpc::DecodeStatus::INCOMPLETE);
	EXPECT_EQ(reader.buffered(), 0u);
}

TEST(FrameTest, KeepsTrailingPartialFrameBuffered) {
	const std::vector<uint8_t> first  = rpc::encode(make_request(CMD_HEARTBEAT, {}));
	const std::vector<uint8_t> second = rpc::encode(make_request(CMD_GET_OWN_ID, {1, 2, 3}));

	std::vector<uint8_t> bytes = first;
	bytes.insert(bytes.end(), second.begin(), second.begin() + 4);

	rpc::FrameReader reader;
	feed(reader, bytes);

	rpc::Frame received;
	ASSERT_EQ(reader.next(received), rpc::DecodeStatus::OK);
	EXPECT_EQ(received.cmd, CMD_HEARTBEAT);
	EXPECT_EQ(reader.next(received), rpc::DecodeStatus::INCOMPLETE);

	reader.append(second.data() + 4, second.size() - 4);
	ASSERT_EQ(reader.next(received), rpc::DecodeStatus::OK);
	EXPECT_EQ(received.cmd, CMD_GET_OWN_ID);
}

TEST(FrameTest, RejectsNegativePayloadLength) {
	std::vector<uint8_t> bytes = rpc::encode(make_request(CMD_HEARTBEAT, {}));
	bytes[4]                   = 0xFF;
	bytes[5]                   = 0xFF; // -1

	rpc::FrameReader reader;
	feed(reader, bytes);

	rpc::Frame received;
	EXPECT_EQ(reader.next(received), rpc::DecodeStatus::INVALID);
}

TEST(FrameTest, RejectsPayloadLengthAboveMaximum) {
	std::vector<uint8_t> bytes    = rpc::encode(make_request(CMD_HEARTBEAT, {}));
	const uint16_t       too_long = static_cast<uint16_t>(rpc::MAX_PAYLOAD + 1);
	bytes[4]                      = static_cast<uint8_t>(too_long & 0xFF);
	bytes[5]                      = static_cast<uint8_t>(too_long >> 8);

	rpc::FrameReader reader;
	feed(reader, bytes);

	rpc::Frame received;
	EXPECT_EQ(reader.next(received), rpc::DecodeStatus::INVALID);
}

TEST(FrameTest, EncodesMaximumPayloadAndRejectsMore) {
	const rpc::Frame largest = make_request(CMD_SEND_PACKET, std::vector<uint8_t>(rpc::MAX_PAYLOAD, 0xAB));
	EXPECT_EQ(rpc::encode(largest).size(), rpc::HEADER_SIZE + rpc::MAX_PAYLOAD);

	const rpc::Frame oversized = make_request(CMD_SEND_PACKET, std::vector<uint8_t>(rpc::MAX_PAYLOAD + 1, 0xAB));
	EXPECT_THROW(static_cast<void>(rpc::encode(oversized)), std::length_error);
}

TEST(FrameTest, ClearDiscardsBufferedBytes) {
	rpc::FrameReader           reader;
	const std::vector<uint8_t> bytes = rpc::encode(make_request(CMD_HEARTBEAT, {}));
	reader.append(bytes.data(), 3);
	EXPECT_EQ(reader.buffered(), 3u);

	reader.clear();
	EXPECT_EQ(reader.buffered(), 0u);
}
