#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "../../src/module/module.h"
#include "../../src/platform/platform.h"
#include "../../src/settings.h"
#include "../../src/world/world.h"

namespace {

	// Checks how the platform escalates against an application that ignores backpressure.
	class OverloadTest : public ::testing::Test {
	protected:
		static constexpr uint8_t MODULE = 1;

		void SetUp() override {
			// Sets every setting a settings.toml would, so the fixture does not depend on test order.
			settings::install_test_defaults();

			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
			settings::set_module_settings(module::ModuleId{TOWER_MODULE_ID}, settings::ModuleSettings{});

			settings::ModuleSettings module_settings{};
			module_settings.active = true;
			settings::set_module_settings(module::ModuleId{MODULE}, module_settings);
			world_.add_module(module::ModuleId{MODULE});

			manager = std::make_unique<platform::Manager>(world_);
		}

		void TearDown() override {
			manager.reset();
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
		}

		int16_t call(uint8_t cmd, uint64_t tick) { return call(cmd, platform::ModuleTime{tick, tick}); }

		int16_t call(uint8_t cmd, platform::ModuleTime time) {
			rpc::Frame frame;
			frame.type                               = MT_REQ;
			frame.cmd                                = cmd;
			frame.call_id                            = 1;
			const std::optional<rpc::Frame> response = manager->handle_request(module::ModuleId{MODULE}, frame, time);
			return response ? response->status : platform::STATUS_OK;
		}

		bool raised(const std::string& kind) {
			for (const platform::Diagnostic& diagnostic : collected) {
				if (diagnostic.kind == kind) {
					return true;
				}
			}
			return false;
		}

		void collect() {
			for (platform::Diagnostic& diagnostic : manager->take_diagnostics()) {
				collected.push_back(diagnostic);
			}
		}

		std::vector<platform::Diagnostic>  collected;
		world::State                       world_;
		std::unique_ptr<platform::Manager> manager;
	};

} // namespace

TEST_F(OverloadTest, RefusesRequestsPastTheBucketLimit) {
	const int limit = platform::capacity_of(platform::Bucket::LASER);

	// Everything inside one simulated second, which is the window the limit is quoted over.
	for (int index = 0; index < limit - 1; index++) {
		EXPECT_EQ(call(CMD_LASER_LEFT_GET, 1), platform::STATUS_OK) << "refused request " << index;
	}

	EXPECT_EQ(call(CMD_LASER_LEFT_GET, 1), platform::STATUS_TRY_AGAIN);
}

TEST_F(OverloadTest, TheWindowSlidesSoASustainedRateBelowTheLimitIsNeverRefused) {
	const int limit = platform::capacity_of(platform::Bucket::LASER);

	// The window slides rather than resetting, so a rate just under the limit is never refused.
	const uint64_t spacing = (platform::RATE_LIMIT_WINDOW_TICKS / static_cast<uint64_t>(limit)) + 1;
	for (uint64_t index = 0; index < 200; index++) {
		EXPECT_EQ(call(CMD_LASER_LEFT_GET, 1 + (index * spacing)), platform::STATUS_OK)
			<< "refused at request " << index;
	}
}

TEST_F(OverloadTest, GlobalTimeAgesRequestsWhileModuleLocalTimeIsFrozen) {
	const int limit = platform::capacity_of(platform::Bucket::LASER);

	for (int index = 0; index < limit - 1; index++) {
		EXPECT_EQ(call(CMD_LASER_LEFT_GET, platform::ModuleTime{1, 1}), platform::STATUS_OK);
	}
	EXPECT_EQ(call(CMD_LASER_LEFT_GET, platform::ModuleTime{platform::RATE_LIMIT_WINDOW_TICKS + 1, 1}),
			  platform::STATUS_OK);
}

TEST_F(OverloadTest, EscalatesFromWarningToPauseToStop) {
	const int limit = platform::capacity_of(platform::Bucket::LASER);

	for (int index = 0; index < limit; index++) {
		call(CMD_LASER_LEFT_GET, 1);
	}
	collect();
	EXPECT_FALSE(raised("warning"));

	// 120 percent of the limit is a complaint.
	for (int index = 0; index < limit / 4; index++) {
		call(CMD_LASER_LEFT_GET, 1);
	}
	collect();
	EXPECT_TRUE(raised("warning"));
	EXPECT_FALSE(raised("pause"));

	// 150 percent takes the application's time away for a second.
	for (int index = 0; index < limit / 2; index++) {
		call(CMD_LASER_LEFT_GET, 1);
	}
	collect();
	EXPECT_TRUE(raised("pause"));
	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::PAUSED);
	EXPECT_TRUE(manager->is_paused(module::ModuleId{MODULE}, 1));
	EXPECT_FALSE(raised("stop"));

	// 200 percent stops it.
	for (int index = 0; index < limit; index++) {
		call(CMD_LASER_LEFT_GET, 1);
	}
	collect();
	EXPECT_TRUE(raised("stop"));
	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::STOPPED);
}

TEST_F(OverloadTest, APauseExpiresAfterASimulatedSecond) {
	const int limit = platform::capacity_of(platform::Bucket::LASER);
	for (int index = 0; index < limit * 3 / 2; index++) {
		call(CMD_LASER_LEFT_GET, 1);
	}
	ASSERT_TRUE(manager->is_paused(module::ModuleId{MODULE}, 1));

	EXPECT_TRUE(manager->is_paused(module::ModuleId{MODULE}, platform::PAUSE_DURATION_TICKS));
	EXPECT_FALSE(manager->is_paused(module::ModuleId{MODULE}, 1 + platform::PAUSE_DURATION_TICKS));

	manager->prepare_tick(1 + platform::PAUSE_DURATION_TICKS);
	collect();
	EXPECT_TRUE(raised("resume"));
	EXPECT_NE(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::PAUSED);
}

TEST_F(OverloadTest, PauseStopsWorldMotionExtendsAndRestoresBelts) {
	module::Module* physical = world_.module(module::ModuleId{MODULE});
	ASSERT_NE(physical, nullptr);
	physical->set_big_belt_speed(module::BeltSpeed{100.0});
	physical->set_small_belt_speed(module::BeltSpeed{-50.0});
	world_.place(7, module::ModuleId{MODULE}, world::Position::RIGHT_LASER, RfidPayload{});
	world_.advance_tick();
	const int64_t moving_position = world_.tub(7)->position_um;
	const int64_t moving_encoder  = physical->big_belt_travel_um();
	ASSERT_NE(moving_encoder, 0);

	const int limit = platform::capacity_of(platform::Bucket::LASER);
	for (int index = 0; index < limit * 3 / 2; index++) {
		call(CMD_LASER_LEFT_GET, 1);
	}
	ASSERT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::PAUSED);
	EXPECT_DOUBLE_EQ(physical->big_belt_speed().raw(), 0.0);
	EXPECT_DOUBLE_EQ(physical->small_belt_speed().raw(), 0.0);

	for (int index = 0; index < 10; index++) world_.advance_tick();
	EXPECT_EQ(world_.tub(7)->position_um, moving_position);
	EXPECT_EQ(physical->big_belt_travel_um(), moving_encoder);

	// A queued Core0 request during the pause repeats enforcement and extends the pending resume.
	call(CMD_LASER_LEFT_GET, 500);
	manager->prepare_tick(1 + platform::PAUSE_DURATION_TICKS);
	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::PAUSED);
	manager->prepare_tick(500 + platform::PAUSE_DURATION_TICKS);
	EXPECT_NE(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::PAUSED);
	EXPECT_DOUBLE_EQ(physical->big_belt_speed().raw(), 100.0);
	EXPECT_DOUBLE_EQ(physical->small_belt_speed().raw(), -50.0);

	// Core0 time aged the old burst out, so the first resumed request cannot immediately repause.
	EXPECT_EQ(call(CMD_LASER_LEFT_GET, 500 + platform::PAUSE_DURATION_TICKS), platform::STATUS_OK);
	EXPECT_NE(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::PAUSED);
}

TEST_F(OverloadTest, TheGlobalBucketCatchesTrafficSpreadAcrossCategories) {
	// The global bucket catches what rotating commands would let past each individual limit.
	const uint8_t commands[] = {CMD_LASER_LEFT_GET, CMD_SERVO_ANGLE_GET, CMD_LED_GET_COLOR, CMD_RFID_CHECK_TAG,
								CMD_GET_OWN_ID};

	int refused = 0;
	for (int index = 0; index < platform::capacity_of(platform::Bucket::GLOBAL) + 10; index++) {
		if (call(commands[index % 5], 1) == platform::STATUS_TRY_AGAIN) {
			refused++;
		}
	}

	EXPECT_GT(refused, 0);
}

TEST_F(OverloadTest, UnknownCommandConsumesGlobalExactlyOnceAndNoCategory) {
	EXPECT_EQ(call(0xFE, 1), platform::STATUS_NO_SYS);
	EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, platform::Bucket::GLOBAL), 1u);
	for (int index = 1; index < static_cast<int>(platform::Bucket::COUNT); index++) {
		EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, static_cast<platform::Bucket>(index)), 0u);
	}
}

TEST_F(OverloadTest, RecognizedRequestConsumesGlobalAndItsCategoryOnce) {
	EXPECT_EQ(call(CMD_LASER_LEFT_GET, 1), platform::STATUS_OK);
	EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, platform::Bucket::GLOBAL), 1u);
	EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, platform::Bucket::LASER), 1u);
}

TEST_F(OverloadTest, PermissionDeniedScheduleConsumesGlobalButNotSchedule) {
	rpc::Frame frame;
	frame.type    = MT_REQ;
	frame.cmd     = CMD_SET_FLIGHT_SCHEDULE;
	frame.call_id = 1;
	const std::optional<rpc::Frame> response =
		manager->handle_request(module::ModuleId{MODULE}, frame, platform::ModuleTime{1, 1});

	ASSERT_TRUE(response.has_value());
	EXPECT_EQ(response->status, platform::STATUS_PERM);
	EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, platform::Bucket::GLOBAL), 1u);
	EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, platform::Bucket::SCHEDULE), 0u);
}

TEST_F(OverloadTest, RawIngressAloneEscalatesGlobalWarningPauseAndStop) {
	const int limit = platform::capacity_of(platform::Bucket::GLOBAL);
	for (int index = 0; index < limit * 2; index++) {
		(void)manager->account_ingress(module::ModuleId{MODULE}, platform::ModuleTime{1, 1});
	}
	collect();

	EXPECT_TRUE(raised("warning"));
	EXPECT_TRUE(raised("pause"));
	EXPECT_TRUE(raised("stop"));
	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::STOPPED);
	EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, platform::Bucket::GLOBAL), 0u);
}

TEST_F(OverloadTest, RateStopResetsStateAndRejectsQueuedSettersAfterCleanup) {
	module::Module* physical = world_.module(module::ModuleId{MODULE});
	ASSERT_NE(physical, nullptr);
	physical->set_big_belt_speed(module::BeltSpeed{100.0});
	physical->set_small_belt_speed(module::BeltSpeed{-100.0});
	physical->set_servo_angle(module::ServoAngle{90.0});
	physical->set_led_color(module::LedColor{0x123456});
	physical->set_left_laser_enabled(true);
	physical->set_right_laser_enabled(true);
	world_.advance_tick();
	ASSERT_NE(physical->big_belt_travel_um(), 0);

	rpc::Frame subscribe;
	subscribe.type    = MT_REQ;
	subscribe.cmd     = CMD_SUBSCRIBE_TO_EVENT;
	subscribe.call_id = 1;
	subscribe.payload = {static_cast<uint8_t>(EVENT_LASER_LEFT_DETECT)};
	ASSERT_TRUE(manager->handle_request(module::ModuleId{MODULE}, subscribe, platform::ModuleTime{1, 1}));
	manager->enqueue(module::ModuleId{MODULE}, rpc::Frame{});

	const int limit = platform::capacity_of(platform::Bucket::LASER);
	for (int index = 0; index < (limit * 2) - 1; index++) call(CMD_LASER_LEFT_GET, 2);

	rpc::Frame trigger;
	trigger.type    = MT_REQ;
	trigger.cmd     = CMD_LASER_LEFT_GET;
	trigger.call_id = 0xCAFE;
	const std::optional<rpc::Frame> terminal =
		manager->handle_request(module::ModuleId{MODULE}, trigger, platform::ModuleTime{2, 2});
	ASSERT_TRUE(terminal.has_value());
	EXPECT_EQ(terminal->status, platform::STATUS_TRY_AGAIN);
	EXPECT_TRUE(terminal->stop_after_delivery);

	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::STOPPED);
	EXPECT_EQ(manager->subscriptions(module::ModuleId{MODULE}), 0u);
	EXPECT_EQ(manager->backlog(module::ModuleId{MODULE}), 0u);
	EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, platform::Bucket::GLOBAL), 0u);
	EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, platform::Bucket::LASER), 0u);
	EXPECT_DOUBLE_EQ(physical->big_belt_speed().raw(), 0.0);
	EXPECT_DOUBLE_EQ(physical->small_belt_speed().raw(), 0.0);
	EXPECT_EQ(physical->big_belt_travel_um(), 0);
	EXPECT_EQ(physical->small_belt_travel_um(), 0);
	EXPECT_DOUBLE_EQ(physical->servo_angle().raw(), 0.0);
	EXPECT_EQ(physical->led_color().raw(), 0u);
	EXPECT_FALSE(physical->is_left_laser_enabled());
	EXPECT_FALSE(physical->is_right_laser_enabled());

	// This models a setter that was already queued behind the triggering handler.
	rpc::Frame stale_setter;
	stale_setter.type    = MT_REQ;
	stale_setter.cmd     = CMD_LASER_LEFT_SET;
	stale_setter.call_id = 0xCAFF;
	stale_setter.payload = {1};
	EXPECT_FALSE(manager->handle_request(module::ModuleId{MODULE}, stale_setter, platform::ModuleTime{3, 3}));
	EXPECT_FALSE(physical->is_left_laser_enabled());
}

TEST_F(OverloadTest, ARefusedRequestHasNoEffect) {
	const int limit = platform::capacity_of(platform::Bucket::SERVO);
	for (int index = 0; index < limit - 1; index++) {
		call(CMD_SERVO_ANGLE_GET, 1);
	}

	rpc::Frame frame;
	frame.type    = MT_REQ;
	frame.cmd     = CMD_SERVO_ANGLE_SET;
	frame.payload = {0, 0, 0x34, 0x43}; // 180 degrees
	const std::optional<rpc::Frame> response =
		manager->handle_request(module::ModuleId{MODULE}, frame, platform::ModuleTime{1, 1});

	ASSERT_TRUE(response.has_value());
	ASSERT_EQ(response->status, platform::STATUS_TRY_AGAIN);
	EXPECT_DOUBLE_EQ(world_.module(module::ModuleId{MODULE})->servo_angle().raw(), 0.0);
}

TEST_F(OverloadTest, ARateRejectedHeartbeatDoesNotActivateOrRefreshLiveness) {
	const int global_limit = platform::capacity_of(platform::Bucket::GLOBAL);
	for (int index = 0; index < global_limit - 1; index++) {
		EXPECT_EQ(call(0xFE, 1), platform::STATUS_NO_SYS);
	}

	rpc::Frame heartbeat;
	heartbeat.type    = MT_REQ;
	heartbeat.cmd     = CMD_HEARTBEAT;
	heartbeat.call_id = 2;
	const std::optional<rpc::Frame> response =
		manager->handle_request(module::ModuleId{MODULE}, heartbeat, platform::ModuleTime{1, 1});

	ASSERT_TRUE(response.has_value());
	EXPECT_EQ(response->status, platform::STATUS_TRY_AGAIN);
	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::ACTIVATING);
	EXPECT_FALSE(manager->is_activated(module::ModuleId{MODULE}));
	EXPECT_EQ(manager->rate_sample_count(module::ModuleId{MODULE}, platform::Bucket::HEARTBEAT), 1u);

	const std::vector<module::ModuleId> stopped = manager->check_supervision(platform::ACTIVATION_DEADLINE_TICKS);
	ASSERT_EQ(stopped.size(), 1u);
	EXPECT_EQ(stopped.front(), module::ModuleId{MODULE});
	EXPECT_FALSE(manager->is_activated(module::ModuleId{MODULE}));
}

TEST_F(OverloadTest, ANonConfirmableRateRejectedHeartbeatDoesNotActivate) {
	const int global_limit = platform::capacity_of(platform::Bucket::GLOBAL);
	for (int index = 0; index < global_limit - 1; index++) {
		EXPECT_EQ(call(0xFE, 1), platform::STATUS_NO_SYS);
	}

	rpc::Frame heartbeat;
	heartbeat.type    = MT_REQ;
	heartbeat.cmd     = CMD_HEARTBEAT;
	heartbeat.call_id = 2;
	heartbeat.flags   = RPC_FLAG_NON_CON;
	EXPECT_FALSE(manager->handle_request(module::ModuleId{MODULE}, heartbeat, platform::ModuleTime{1, 1}));
	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::ACTIVATING);
	EXPECT_FALSE(manager->is_activated(module::ModuleId{MODULE}));
}

TEST_F(OverloadTest, ARateRejectedHeartbeatDoesNotClearWarningOrRefreshRunningLiveness) {
	call(CMD_HEARTBEAT, 100);
	manager->check_supervision(100 + platform::HEARTBEAT_WARNING_TICKS + 1);
	(void)manager->take_diagnostics();

	const int global_limit = platform::capacity_of(platform::Bucket::GLOBAL);
	for (int index = 0; index < global_limit - 2; index++) {
		EXPECT_EQ(call(0xFE, 600), platform::STATUS_NO_SYS);
	}
	rpc::Frame heartbeat;
	heartbeat.type    = MT_REQ;
	heartbeat.cmd     = CMD_HEARTBEAT;
	heartbeat.call_id = 2;
	const std::optional<rpc::Frame> response =
		manager->handle_request(module::ModuleId{MODULE}, heartbeat, platform::ModuleTime{600, 600});
	ASSERT_TRUE(response.has_value());
	EXPECT_EQ(response->status, platform::STATUS_TRY_AGAIN);

	manager->check_supervision(601);
	EXPECT_TRUE(manager->take_diagnostics().empty());

	const std::vector<module::ModuleId> stopped =
		manager->check_supervision(100 + platform::HEARTBEAT_FAILURE_TICKS + 1);
	ASSERT_EQ(stopped.size(), 1u);
	EXPECT_EQ(stopped.front(), module::ModuleId{MODULE});
}

TEST_F(OverloadTest, AnApplicationThatNeverHeartbeatsStopsAtItsActivationDeadline) {
	EXPECT_NO_THROW(manager->check_supervision(platform::ACTIVATION_DEADLINE_TICKS - 1));
	const std::vector<module::ModuleId> stopped = manager->check_supervision(platform::ACTIVATION_DEADLINE_TICKS);
	ASSERT_EQ(stopped.size(), 1u);
	EXPECT_EQ(stopped.front(), module::ModuleId{MODULE});
	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::STOPPED);
	EXPECT_FALSE(manager->is_activated(module::ModuleId{MODULE}));
	EXPECT_NE(manager->stop_reason(module::ModuleId{MODULE}).find("activation deadline"), std::string::npos);
}

TEST_F(OverloadTest, AnApplicationThatStopsHeartbeatingIsWarnedThenSafeStopped) {
	call(CMD_HEARTBEAT, 100);
	module::Module* physical = world_.module(module::ModuleId{MODULE});
	ASSERT_NE(physical, nullptr);
	physical->set_big_belt_speed(module::BeltSpeed{100.0});
	physical->set_small_belt_speed(module::BeltSpeed{-100.0});
	physical->set_left_laser_enabled(true);
	physical->set_right_laser_enabled(true);
	rpc::Frame subscribe;
	subscribe.type    = MT_REQ;
	subscribe.cmd     = CMD_SUBSCRIBE_TO_EVENT;
	subscribe.call_id = 2;
	subscribe.payload = {
		static_cast<uint8_t>(EVENT_LASER_LEFT_DETECT), static_cast<uint8_t>(EVENT_LASER_LEFT_DETECT >> 8),
		static_cast<uint8_t>(EVENT_LASER_LEFT_DETECT >> 16), static_cast<uint8_t>(EVENT_LASER_LEFT_DETECT >> 24)};
	ASSERT_TRUE(manager->handle_request(module::ModuleId{MODULE}, subscribe, platform::ModuleTime{100, 100}));
	ASSERT_NE(manager->subscriptions(module::ModuleId{MODULE}), 0u);
	manager->enqueue(module::ModuleId{MODULE}, rpc::Frame{});

	EXPECT_NO_THROW(manager->check_supervision(100 + platform::HEARTBEAT_WARNING_TICKS));
	collect();
	EXPECT_FALSE(raised("heartbeat_late"));

	EXPECT_NO_THROW(manager->check_supervision(101 + platform::HEARTBEAT_WARNING_TICKS));
	collect();
	EXPECT_TRUE(raised("heartbeat_late"));

	const std::vector<module::ModuleId> stopped = manager->check_supervision(101 + platform::HEARTBEAT_FAILURE_TICKS);
	ASSERT_EQ(stopped.size(), 1u);
	EXPECT_EQ(stopped.front(), module::ModuleId{MODULE});
	EXPECT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::STOPPED);
	EXPECT_FALSE(manager->is_activated(module::ModuleId{MODULE}));
	EXPECT_EQ(manager->subscriptions(module::ModuleId{MODULE}), 0u);
	EXPECT_EQ(manager->backlog(module::ModuleId{MODULE}), 0u);
	EXPECT_DOUBLE_EQ(physical->big_belt_speed().raw(), 0.0);
	EXPECT_DOUBLE_EQ(physical->small_belt_speed().raw(), 0.0);
	EXPECT_FALSE(physical->is_left_laser_enabled());
	EXPECT_FALSE(physical->is_right_laser_enabled());
	rpc::Frame stale;
	stale.type    = MT_REQ;
	stale.cmd     = CMD_LASER_LEFT_SET;
	stale.call_id = 3;
	stale.payload = {1};
	EXPECT_FALSE(manager->handle_request(
		module::ModuleId{MODULE}, stale,
		platform::ModuleTime{102 + platform::HEARTBEAT_FAILURE_TICKS, 102 + platform::HEARTBEAT_FAILURE_TICKS}));
	EXPECT_FALSE(physical->is_left_laser_enabled());
	collect();
	EXPECT_TRUE(raised("stop"));
}

TEST_F(OverloadTest, AHeartbeatClearsTheWarning) {
	call(CMD_HEARTBEAT, 100);
	manager->check_supervision(101 + platform::HEARTBEAT_WARNING_TICKS);
	collected.clear();
	(void)manager->take_diagnostics();

	call(CMD_HEARTBEAT, 200 + platform::HEARTBEAT_WARNING_TICKS);
	manager->check_supervision(200 + platform::HEARTBEAT_WARNING_TICKS);
	collect();
	EXPECT_FALSE(raised("heartbeat_late"));

	// And a later silence warns again rather than staying quiet because it warned once.
	manager->check_supervision(201 + (2 * platform::HEARTBEAT_WARNING_TICKS));
	collect();
	EXPECT_TRUE(raised("heartbeat_late"));
}

TEST_F(OverloadTest, APausedApplicationIsNotExpectedToHeartbeat) {
	call(CMD_HEARTBEAT, 1);

	const int limit = platform::capacity_of(platform::Bucket::LASER);
	for (int index = 0; index < limit * 3 / 2; index++) {
		call(CMD_LASER_LEFT_GET, 1);
	}
	ASSERT_EQ(manager->lifecycle(module::ModuleId{MODULE}), platform::Lifecycle::PAUSED);

	// It has not been given time to send one, so its silence must not count against it.
	EXPECT_NO_THROW(manager->check_supervision(1 + platform::HEARTBEAT_FAILURE_TICKS));
}
