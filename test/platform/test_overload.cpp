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

TEST_F(OverloadTest, GlobalTimeDoesNotAgeRequestsOutOfAModuleLocalRateWindow) {
	const int limit = platform::capacity_of(platform::Bucket::LASER);

	for (int index = 0; index < limit - 1; index++) {
		EXPECT_EQ(call(CMD_LASER_LEFT_GET, platform::ModuleTime{static_cast<uint64_t>(index) * 10'000, 1}),
				  platform::STATUS_OK);
	}
	EXPECT_EQ(call(CMD_LASER_LEFT_GET, platform::ModuleTime{1'000'000, 1}), platform::STATUS_TRY_AGAIN);
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

TEST_F(OverloadTest, AnApplicationThatNeverHeartbeatsFailsItsActivationDeadline) {
	EXPECT_NO_THROW(manager->check_supervision(platform::ACTIVATION_DEADLINE_TICKS - 1));
	EXPECT_THROW(manager->check_supervision(platform::ACTIVATION_DEADLINE_TICKS), std::runtime_error);
}

TEST_F(OverloadTest, AnApplicationThatStopsHeartbeatingIsWarnedThenFailed) {
	call(CMD_HEARTBEAT, 100);

	EXPECT_NO_THROW(manager->check_supervision(100 + platform::HEARTBEAT_WARNING_TICKS));
	collect();
	EXPECT_FALSE(raised("heartbeat_late"));

	EXPECT_NO_THROW(manager->check_supervision(101 + platform::HEARTBEAT_WARNING_TICKS));
	collect();
	EXPECT_TRUE(raised("heartbeat_late"));

	EXPECT_THROW(manager->check_supervision(101 + platform::HEARTBEAT_FAILURE_TICKS), std::runtime_error);
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
