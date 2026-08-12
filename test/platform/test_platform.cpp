#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "../../src/module/module.h"
#include "../../src/net/network.h"
#include "../../src/platform/platform.h"
#include "../../src/settings.h"
#include "../../src/world/world.h"

namespace {

	std::vector<uint8_t> float_bytes(float value) {
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return {static_cast<uint8_t>(bits & 0xFF), static_cast<uint8_t>((bits >> 8) & 0xFF),
				static_cast<uint8_t>((bits >> 16) & 0xFF), static_cast<uint8_t>((bits >> 24) & 0xFF)};
	}

	float bytes_to_float(const std::vector<uint8_t>& payload) {
		const uint32_t bits  = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8) |
							   (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
		float          value = 0;
		std::memcpy(&value, &bits, sizeof(value));
		return value;
	}

	rpc::Frame request(uint8_t cmd, std::vector<uint8_t> payload = {}) {
		rpc::Frame frame;
		frame.type    = MT_REQ;
		frame.cmd     = cmd;
		frame.call_id = 0x8001;
		frame.payload = std::move(payload);
		return frame;
	}

	class PlatformTest : public ::testing::Test {
	protected:
		static constexpr uint8_t MODULE    = 1;
		static constexpr uint8_t NEIGHBOUR = 2;

		// Module settings are process-global, so the fixture clears them rather than trusting them.
		static void clear_module_settings() {
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
			settings::set_module_settings(module::ModuleId{TOWER_MODULE_ID}, settings::ModuleSettings{});
		}

		void SetUp() override {
			clear_module_settings();
			world::set_calibration_config(world::CalibrationConfig{});
			world::set_calibration_override(world::Calibration::nominal());

			// The manager reads network settings at build time, so the fixture must set them itself.
			settings::set_setting("network_fixed_delay_ms", 5);
			settings::set_setting("network_packet_loss_ppm", 0);
			settings::set_setting("network_very_late_ppm", 0);
			settings::set_setting("network_very_late_delay_ms", 1100);
			settings::set_setting("network_log_extra_delay_ms", 0);
			settings::set_setting("seed", 1);

			settings::ModuleSettings first{};
			first.active = true;
			first.type   = "default";
			first.left   = module::ModuleId{NEIGHBOUR};
			settings::set_module_settings(module::ModuleId{MODULE}, first);

			settings::ModuleSettings second{};
			second.active = true;
			second.type   = "gate";
			second.right  = module::ModuleId{MODULE};
			settings::set_module_settings(module::ModuleId{NEIGHBOUR}, second);

			settings::ModuleSettings tower{};
			tower.active = true;
			tower.type   = "tower";
			settings::set_module_settings(module::ModuleId{TOWER_MODULE_ID}, tower);

			for (const uint8_t id : {MODULE, NEIGHBOUR, TOWER_MODULE_ID}) {
				world_.add_module(module::ModuleId{id});
			}

			manager = std::make_unique<platform::Manager>(world_);
		}

		void TearDown() override {
			manager.reset();
			world::clear_calibration_seed();
			world::set_calibration_config(world::CalibrationConfig{});
			clear_module_settings();
		}

		// Advances a full rate-limit window per call, so tests exercise commands, not the limiter.
		rpc::Frame call(uint8_t cmd, std::vector<uint8_t> payload = {}, uint8_t id = MODULE) {
			tick_ += platform::RATE_LIMIT_WINDOW_TICKS;
			const std::optional<rpc::Frame> response =
				manager->handle_request(module::ModuleId{id}, request(cmd, std::move(payload)), tick_);
			EXPECT_TRUE(response.has_value());
			return response.value_or(rpc::Frame{});
		}

		uint64_t tick_ = 0;

		module::Module* hardware() { return world_.module(module::ModuleId{MODULE}); }

		// Drains a module's whole backlog, a tick's worth at a time.
		std::vector<rpc::Frame> take_all(uint8_t id = MODULE) {
			std::vector<rpc::Frame> frames;
			while (true) {
				const std::vector<rpc::Frame> batch =
					manager->take_outbound(module::ModuleId{id}, childsync::MAILBOX_CAPACITY);
				if (batch.empty()) {
					return frames;
				}
				frames.insert(frames.end(), batch.begin(), batch.end());
			}
		}

		world::State                       world_;
		std::unique_ptr<platform::Manager> manager;
	};

} // namespace

TEST_F(PlatformTest, RespondsWithTheRequestsCommandAndCallId) {
	const rpc::Frame response = call(CMD_GET_OWN_ID);

	EXPECT_EQ(response.type, MT_RESP);
	EXPECT_EQ(response.cmd, CMD_GET_OWN_ID);
	EXPECT_EQ(response.call_id, 0x8001);
	EXPECT_EQ(response.status, platform::STATUS_OK);
	ASSERT_EQ(response.payload.size(), 1u);
	EXPECT_EQ(response.payload[0], MODULE);
}

TEST_F(PlatformTest, DoesNotAnswerNonConfirmableRequests) {
	rpc::Frame frame = request(CMD_LOG, {'h', 'i'});
	frame.flags      = RPC_FLAG_NON_CON;

	EXPECT_FALSE(manager->handle_request(module::ModuleId{MODULE}, frame).has_value());
	// The side effect still happens once delivered; only the response is withheld.
	manager->advance(net::Config{}.fixed_delay_ms);
	ASSERT_EQ(manager->logs(module::ModuleId{MODULE}).size(), 1u);
	EXPECT_EQ(manager->logs(module::ModuleId{MODULE})[0], "hi");
}

TEST_F(PlatformTest, LaserReportsClearOnlyWhilePowered) {
	// A laser that was never switched on has no beam to report on.
	EXPECT_EQ(call(CMD_LASER_LEFT_DETECT).payload[0], 0);

	EXPECT_EQ(call(CMD_LASER_LEFT_SET, {1}).status, platform::STATUS_OK);
	EXPECT_EQ(call(CMD_LASER_LEFT_GET).payload[0], 1);
	EXPECT_EQ(call(CMD_LASER_LEFT_DETECT).payload[0], 1);

	hardware()->set_left_laser_blocked(true);
	EXPECT_EQ(call(CMD_LASER_LEFT_DETECT).payload[0], 0);

	hardware()->set_left_laser_blocked(false);
	EXPECT_EQ(call(CMD_LASER_LEFT_DETECT).payload[0], 1);

	EXPECT_EQ(call(CMD_LASER_LEFT_SET, {0}).status, platform::STATUS_OK);
	EXPECT_EQ(call(CMD_LASER_LEFT_DETECT).payload[0], 0);
}

TEST_F(PlatformTest, RejectsMalformedLaserSet) {
	EXPECT_EQ(call(CMD_LASER_RIGHT_SET, {}).status, platform::STATUS_INVALID);
	EXPECT_EQ(call(CMD_LASER_RIGHT_SET, {1, 2}).status, platform::STATUS_INVALID);
}

TEST_F(PlatformTest, SetsAndReadsBeltSpeeds) {
	EXPECT_EQ(call(CMD_BELT_BIG_SET_SPEED, float_bytes(-42.5F)).status, platform::STATUS_OK);
	EXPECT_FLOAT_EQ(bytes_to_float(call(CMD_BELT_BIG_GET_SPEED).payload), -42.5F);

	EXPECT_EQ(call(CMD_BELT_SMALL_SET_SPEED, float_bytes(100.0F)).status, platform::STATUS_OK);
	EXPECT_FLOAT_EQ(bytes_to_float(call(CMD_BELT_SMALL_GET_SPEED).payload), 100.0F);
}

TEST_F(PlatformTest, RejectsOutOfRangeBeltSpeed) {
	// Refused at the boundary rather than allowed to abort the simulation.
	EXPECT_EQ(call(CMD_BELT_BIG_SET_SPEED, float_bytes(100.1F)).status, platform::STATUS_INVALID);
	EXPECT_EQ(call(CMD_BELT_BIG_SET_SPEED, float_bytes(-1000.0F)).status, platform::STATUS_INVALID);
	EXPECT_EQ(call(CMD_BELT_BIG_SET_SPEED, {1, 2}).status, platform::STATUS_INVALID);
}

TEST_F(PlatformTest, SetsAndReadsServoAngle) {
	EXPECT_EQ(call(CMD_SERVO_ANGLE_SET, float_bytes(135.0F)).status, platform::STATUS_OK);
	EXPECT_FLOAT_EQ(bytes_to_float(call(CMD_SERVO_ANGLE_GET).payload), 135.0F);

	EXPECT_EQ(call(CMD_SERVO_ANGLE_SET, float_bytes(181.0F)).status, platform::STATUS_INVALID);
	EXPECT_EQ(call(CMD_SERVO_ANGLE_SET, float_bytes(-1.0F)).status, platform::STATUS_INVALID);
}

TEST_F(PlatformTest, SetsAndReadsLedColor) {
	EXPECT_EQ(call(CMD_LED_SET_COLOR, {0x3F, 0x00, 0x00, 0x00}).status, platform::STATUS_OK);
	EXPECT_EQ(call(CMD_LED_GET_COLOR).payload, std::vector<uint8_t>({0x3F, 0x00, 0x00, 0x00}));
	EXPECT_EQ(call(CMD_LED_SET_COLOR, {1, 2}).status, platform::STATUS_INVALID);
}

TEST_F(PlatformTest, ReadsAndWritesRfidOnlyWhileATagIsPresent) {
	EXPECT_EQ(call(CMD_RFID_CHECK_TAG).payload[0], 0);

	// An empty reader answers with an empty UID rather than refusing; no tag is not an error.
	EXPECT_EQ(call(CMD_RFID_GET_UID).status, platform::STATUS_OK);
	EXPECT_TRUE(call(CMD_RFID_GET_UID).payload.empty());

	// A data block still refuses, because there is no value it could honestly return.
	EXPECT_EQ(call(CMD_RFID_READ_DATA_BLOCK, {0}).status, platform::STATUS_NO_ENTRY);
	EXPECT_EQ(call(CMD_RFID_READ_DATA_BLOCK, {32}).status, platform::STATUS_RANGE);
	EXPECT_EQ(call(CMD_RFID_WRITE_DATA_BLOCK, {32, 1, 2, 3, 4}).status, platform::STATUS_RANGE);

	RfidPayload data{};
	data[2]  = {9, 8, 7, 6};
	data[31] = {0xAA, 0xBB, 0xCC, 0xDD};
	hardware()->set_rfid_tag({1, 2, 3, 4, 5, 6, 7}, data);

	EXPECT_EQ(call(CMD_RFID_CHECK_TAG).payload[0], 1);
	EXPECT_EQ(call(CMD_RFID_GET_UID).payload, std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7}));
	EXPECT_EQ(call(CMD_RFID_READ_DATA_BLOCK, {2}).payload, std::vector<uint8_t>({9, 8, 7, 6}));
	EXPECT_EQ(call(CMD_RFID_READ_DATA_BLOCK, {31}).payload, std::vector<uint8_t>({0xAA, 0xBB, 0xCC, 0xDD}));

	EXPECT_EQ(call(CMD_RFID_WRITE_DATA_BLOCK, {2, 1, 1, 1, 1}).status, platform::STATUS_OK);
	EXPECT_EQ(call(CMD_RFID_READ_DATA_BLOCK, {2}).payload, std::vector<uint8_t>({1, 1, 1, 1}));

	EXPECT_EQ(call(CMD_RFID_READ_DATA_BLOCK, {RFID_PAYLOAD_PAGE_COUNT}).status, platform::STATUS_RANGE);
	EXPECT_EQ(call(CMD_RFID_WRITE_DATA_BLOCK, {RFID_PAYLOAD_PAGE_COUNT, 1, 2, 3, 4}).status, platform::STATUS_RANGE);
	EXPECT_EQ(call(CMD_RFID_WRITE_DATA_BLOCK, {0, 1}).status, platform::STATUS_INVALID);
}

TEST_F(PlatformTest, RfidWritesSurviveTheNextWorldTick) {
	RfidPayload data{};
	data[2] = {9, 8, 7, 6};
	world_.place(7, module::ModuleId{MODULE}, world::Position::ENTRY, data);

	EXPECT_EQ(call(CMD_RFID_WRITE_DATA_BLOCK, {2, 1, 2, 3, 4}).status, platform::STATUS_OK);
	world_.advance_tick();

	EXPECT_EQ(call(CMD_RFID_READ_DATA_BLOCK, {2}).payload, std::vector<uint8_t>({1, 2, 3, 4}));
	ASSERT_NE(world_.tub(7), nullptr);
	EXPECT_EQ(world_.tub(7)->payload[2], (RfidPage{1, 2, 3, 4}));
}

TEST_F(PlatformTest, ServesTheNetworkMapInTopologyTextForm) {
	const rpc::Frame  response = call(CMD_GET_NETWORK_MAP);
	const std::string map(response.payload.begin(), response.payload.end());

	EXPECT_EQ(map, "0\nM1,tdefault,a2,b0,c0\nM2,tgate,a0,b0,c1\nM255,ttower,a0,b0,c0\n");
}

TEST_F(PlatformTest, ServesTheModuleList) {
	const rpc::Frame response = call(CMD_GET_MODULE_LIST);

	ASSERT_EQ(response.payload.size(), 2u + (3u * 5u));
	EXPECT_EQ(response.payload[0], 3);
	EXPECT_EQ(response.payload[1], 0);
	EXPECT_EQ(response.payload[2], MODULE);
	EXPECT_EQ(response.payload[7], NEIGHBOUR);
	EXPECT_EQ(response.payload[12], TOWER_MODULE_ID);
}

TEST_F(PlatformTest, TracksSubscriptions) {
	EXPECT_EQ(manager->subscriptions(module::ModuleId{MODULE}), 0u);

	EXPECT_EQ(call(CMD_SUBSCRIBE_TO_EVENT, {EVENT_LASER_LEFT_DETECT | EVENT_RFID_DETECT}).status, platform::STATUS_OK);
	EXPECT_EQ(manager->subscriptions(module::ModuleId{MODULE}),
			  static_cast<uint32_t>(EVENT_LASER_LEFT_DETECT | EVENT_RFID_DETECT));

	EXPECT_EQ(call(CMD_UNSUBSCRIBE_FROM_EVENT, {EVENT_RFID_DETECT}).status, platform::STATUS_OK);
	EXPECT_EQ(manager->subscriptions(module::ModuleId{MODULE}), static_cast<uint32_t>(EVENT_LASER_LEFT_DETECT));
}

TEST_F(PlatformTest, RefusesSubscriptionToUnsupportedEvents) {
	// Encoder events belong to commands this version does not implement.
	EXPECT_EQ(call(CMD_SUBSCRIBE_TO_EVENT, {EVENT_ENCODER_LONG_STOPPED}).status, platform::STATUS_INVALID);
	EXPECT_EQ(call(CMD_SUBSCRIBE_TO_EVENT, {}).status, platform::STATUS_INVALID);
	EXPECT_EQ(manager->subscriptions(module::ModuleId{MODULE}), 0u);
}

TEST_F(PlatformTest, RefusesHardwareAccessFromTheTower) {
	EXPECT_EQ(call(CMD_SERVO_ANGLE_SET, float_bytes(90.0F), TOWER_MODULE_ID).status, platform::STATUS_PERM);
	EXPECT_EQ(call(CMD_LASER_LEFT_DETECT, {}, TOWER_MODULE_ID).status, platform::STATUS_PERM);
	EXPECT_EQ(call(CMD_SUBSCRIBE_TO_EVENT, {EVENT_LASER_LEFT_DETECT}, TOWER_MODULE_ID).status, platform::STATUS_PERM);

	// The tower still takes part in the logical network.
	EXPECT_EQ(call(CMD_SUBSCRIBE_TO_EVENT, {EVENT_MESSAGE_RECEIVED}, TOWER_MODULE_ID).status, platform::STATUS_OK);
	EXPECT_EQ(call(CMD_GET_NETWORK_MAP, {}, TOWER_MODULE_ID).status, platform::STATUS_OK);
}

TEST_F(PlatformTest, AcceptsTheFlightBoardOnlyFromTheTower) {
	// A header declaring one arrival and no departures, followed by one flight.
	std::vector<uint8_t> board(sizeof(quercus_flight_board_t) + sizeof(quercus_flight_t), 0);
	board[2] = 1;

	EXPECT_EQ(call(CMD_SET_FLIGHT_SCHEDULE, board).status, platform::STATUS_PERM);
	EXPECT_TRUE(manager->flight_schedule().empty());

	EXPECT_EQ(call(CMD_SET_FLIGHT_SCHEDULE, board, TOWER_MODULE_ID).status, platform::STATUS_OK);
	EXPECT_EQ(manager->flight_schedule(), board);

	board.pop_back();
	EXPECT_EQ(call(CMD_SET_FLIGHT_SCHEDULE, board, TOWER_MODULE_ID).status, platform::STATUS_INVALID);
}

TEST_F(PlatformTest, ReportsAnUnknownCommandAsUnimplemented) {
	EXPECT_EQ(call(0xAB).status, platform::STATUS_NO_SYS);
}

TEST_F(PlatformTest, ReadsTheEncodersInTheWidthAnApplicationExpects) {
	// An encoder count is a `long`, four bytes on both the Pico and the Native Simulator.
	const rpc::Frame count = call(CMD_BELT_BIG_GET_ENCODER_COUNT);
	EXPECT_EQ(count.status, platform::STATUS_OK);
	EXPECT_EQ(count.payload.size(), 4u);

	// A frequency is a double, which is eight bytes everywhere.
	const rpc::Frame freq = call(CMD_BELT_SMALL_GET_ENCODER_FREQ);
	EXPECT_EQ(freq.status, platform::STATUS_OK);
	EXPECT_EQ(freq.payload.size(), 8u);

	for (const uint8_t cmd : {CMD_CV_SENSOR_GET_CURRENT, CMD_CV_SENSOR_GET_POWER, CMD_CV_SENSOR_GET_VOLTAGE}) {
		const rpc::Frame reading = call(cmd);
		EXPECT_EQ(reading.status, platform::STATUS_OK);
		EXPECT_EQ(reading.payload.size(), 4u);
	}
}

TEST_F(PlatformTest, ActivatesOnTheFirstHeartbeat) {
	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::ACTIVATING);
	EXPECT_FALSE(manager->is_activated(module::ModuleId{MODULE}));

	EXPECT_EQ(call(CMD_HEARTBEAT).status, platform::STATUS_OK);

	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::RUNNING);
	EXPECT_TRUE(manager->is_activated(module::ModuleId{MODULE}));
}

TEST_F(PlatformTest, RecordsApplicationDiagnostics) {
	EXPECT_EQ(call(CMD_LOG, {'b', 'o', 'o', 't'}).status, platform::STATUS_OK);
	EXPECT_EQ(call(CMD_PANIC_REPORT, {'b', 'a', 'd'}).status, platform::STATUS_OK);

	// Logs and panics travel their own delivery model, same as data packets do.
	manager->advance(tick_ + net::Config{}.fixed_delay_ms);

	ASSERT_EQ(manager->logs(module::ModuleId{MODULE}).size(), 1u);
	EXPECT_EQ(manager->logs(module::ModuleId{MODULE})[0], "boot");
	ASSERT_EQ(manager->panics(module::ModuleId{MODULE}).size(), 1u);
	EXPECT_EQ(manager->panics(module::ModuleId{MODULE})[0], "bad");
}

TEST_F(PlatformTest, PublishesLaserEventsOnlyOnPositiveEdgeAndOnlyWhenSubscribed) {
	call(CMD_LASER_LEFT_SET, {1});

	// Not subscribed yet, so the transition to a clear beam produces nothing.
	manager->advance(0);
	EXPECT_TRUE(take_all().empty());

	call(CMD_SUBSCRIBE_TO_EVENT, {EVENT_LASER_LEFT_DETECT});

	// Steady state produces nothing either; only a clear-to-blocked transition is an event.
	manager->advance(1);
	EXPECT_TRUE(take_all().empty());

	hardware()->set_left_laser_blocked(true);
	manager->advance(2);
	std::vector<rpc::Frame> events = take_all();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].type, MT_EVENT);
	EXPECT_EQ(events[0].cmd, EVENT_LASER_LEFT_DETECT);

	manager->advance(30);
	EXPECT_TRUE(take_all().empty());

	hardware()->set_left_laser_blocked(false);
	manager->advance(4);
	EXPECT_TRUE(take_all().empty());

	manager->advance(30);
	EXPECT_TRUE(take_all().empty());

	hardware()->set_left_laser_blocked(true);
	manager->advance(30);
	ASSERT_EQ(take_all().size(), 1u);
}

TEST_F(PlatformTest, PublishesRfidDetectWithTheTagUid) {
	call(CMD_SUBSCRIBE_TO_EVENT, {EVENT_RFID_DETECT});

	RfidPayload data{};
	hardware()->set_rfid_tag({1, 2, 3, 4, 5, 6, 7}, data);

	manager->advance(0);
	std::vector<rpc::Frame> events = take_all();
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].cmd, EVENT_RFID_DETECT);
	EXPECT_EQ(events[0].payload, std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7}));

	// A tag leaving the reader is not an event; an application notices by asking.
	hardware()->clear_rfid_tag();
	manager->advance(1);
	EXPECT_TRUE(take_all().empty());
}

TEST_F(PlatformTest, DeliversPacketsAsEventsAfterTheNetworkDelay) {
	call(CMD_SUBSCRIBE_TO_EVENT, {EVENT_MESSAGE_RECEIVED}, NEIGHBOUR);

	EXPECT_EQ(call(CMD_SEND_PACKET, {NEIGHBOUR, 'h', 'i'}, MODULE).status, platform::STATUS_OK);
	const uint64_t sent_at = tick_;

	for (uint64_t tick = sent_at; tick < sent_at + net::Config{}.fixed_delay_ms; tick++) {
		manager->advance(tick);
		EXPECT_TRUE(take_all(NEIGHBOUR).empty()) << "delivered early at tick " << tick;
	}

	manager->advance(sent_at + net::Config{}.fixed_delay_ms);
	std::vector<rpc::Frame> events = take_all(NEIGHBOUR);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].cmd, EVENT_MESSAGE_RECEIVED);
	// The destination byte is addressing, not payload, so the application sees only its data.
	EXPECT_EQ(events[0].payload, std::vector<uint8_t>({'h', 'i'}));
}

TEST_F(PlatformTest, RefusesPacketsToUnknownModules) {
	EXPECT_EQ(call(CMD_SEND_PACKET, {99, 'x'}, MODULE).status, platform::STATUS_NO_ENTRY);
	EXPECT_EQ(call(CMD_SEND_PACKET, {}, MODULE).status, platform::STATUS_NO_ENTRY);
}

TEST_F(PlatformTest, BroadcastNeverReachesSender) {
	call(CMD_SUBSCRIBE_TO_EVENT, {EVENT_MESSAGE_RECEIVED}, MODULE);
	call(CMD_SUBSCRIBE_TO_EVENT, {EVENT_MESSAGE_RECEIVED}, NEIGHBOUR);

	EXPECT_EQ(call(CMD_BROADCAST_PACKET, {'Q', 'F', 'B'}, MODULE).status, platform::STATUS_OK);
	manager->advance(tick_ + net::Config{}.fixed_delay_ms);

	std::vector<rpc::Frame> events = take_all(MODULE);
	EXPECT_TRUE(events.empty());

	events = take_all(NEIGHBOUR);
	ASSERT_EQ(events.size(), 1u);
	EXPECT_EQ(events[0].payload, std::vector<uint8_t>({'Q', 'F', 'B'}));
}

TEST_F(PlatformTest, CapsOutboundFramesPerTickAndKeepsTheRestInOrder) {
	// Queued directly, since what is under test here is delivery pacing, not frame creation.
	for (uint8_t index = 0; index < 8; index++) {
		rpc::Frame frame;
		frame.type    = MT_EVENT;
		frame.cmd     = EVENT_MESSAGE_RECEIVED;
		frame.payload = {index};
		manager->enqueue(module::ModuleId{NEIGHBOUR}, frame);
	}

	EXPECT_EQ(manager->backlog(module::ModuleId{NEIGHBOUR}), 8u);

	uint8_t expected = 0;
	for (int round = 0; round < 3; round++) {
		const std::vector<rpc::Frame> frames =
			manager->take_outbound(module::ModuleId{NEIGHBOUR}, childsync::MAILBOX_CAPACITY);
		EXPECT_LE(frames.size(), platform::MAX_OUTBOUND_PER_TICK);
		for (const rpc::Frame& frame : frames) {
			ASSERT_EQ(frame.payload.size(), 1u);
			EXPECT_EQ(frame.payload[0], expected) << "delivery order changed";
			expected++;
		}
	}

	EXPECT_EQ(expected, 8);
	EXPECT_EQ(manager->backlog(module::ModuleId{NEIGHBOUR}), 0u);
}

TEST_F(PlatformTest, IgnoresRequestsFromAModuleItDoesNotOwn) {
	rpc::Frame frame = request(CMD_GET_OWN_ID);
	EXPECT_FALSE(manager->handle_request(module::ModuleId{5}, frame).has_value());
	EXPECT_EQ(manager->lifecycle(module::ModuleId{5}), platform::Lifecycle::STOPPED);
}
