#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sched.h>
#include <sstream>
#include <string>

#include "../../src/event/events.h"
#include "../../src/ltl/parser.h"
#include "../../src/module/module.h"
#include "../../src/settings.h"
#include "../../src/sim/runtime.h"

namespace fs = std::filesystem;

namespace {
	TEST(Runtime, PinsLockstepToOneCpu) {
		cpu_set_t original;
		ASSERT_EQ(::sched_getaffinity(0, sizeof(original), &original), 0);

		sim::pin_to_one_cpu();
		cpu_set_t selected;
		const int result = ::sched_getaffinity(0, sizeof(selected), &selected);
		EXPECT_EQ(result, 0);
		if (result == 0) {
			EXPECT_EQ(CPU_COUNT(&selected), 1);
		}

		EXPECT_EQ(::sched_setaffinity(0, sizeof(original), &original), 0);
	}

	// Runs the whole simulation-side loop against real child processes and shared mailboxes.
	class RuntimeTest : public ::testing::Test {
	protected:
		static constexpr uint8_t FIRST  = 1;
		static constexpr uint8_t SECOND = 2;

		static void clear_fake_child_environment() {
			::unsetenv("FAKE_CHILD_HEARTBEAT_EVERY");
			::unsetenv("FAKE_CHILD_HEARTBEAT_EVERY_1");
			::unsetenv("FAKE_CHILD_HEARTBEAT_EVERY_2");
			::unsetenv("FAKE_CHILD_HEARTBEAT_UNTIL");
			::unsetenv("FAKE_CHILD_HEARTBEAT_UNTIL_1");
			::unsetenv("FAKE_CHILD_HEARTBEAT_UNTIL_2");
			::unsetenv("FAKE_CHILD_SUBSCRIBE");
			::unsetenv("FAKE_CHILD_EXIT_AFTER");
			::unsetenv("FAKE_CHILD_QUERY_TICK");
			::unsetenv("FAKE_CHILD_INGRESS_PROBE");
			::unsetenv("FAKE_CHILD_INGRESS_PROBE_1");
			::unsetenv("FAKE_CHILD_INGRESS_PROBE_2");
			::unsetenv("FAKE_CHILD_RATE_BURST");
			::unsetenv("FAKE_CHILD_RATE_BURST_1");
			::unsetenv("FAKE_CHILD_RATE_BURST_2");
			::unsetenv("FAKE_CHILD_STARTUP_RPC");
			::unsetenv("FAKE_CHILD_RECORD");
			::unsetenv("FAKE_CHILD_RECORD_TICKS");
			::unsetenv("FAKE_CHILD_GRANT_RECORD");
			::unsetenv("FAKE_CHILD_STATUS_RECORD");
			::unsetenv("FAKE_CHILD_MUTANT");
			::unsetenv("FAKE_CHILD_REPLY_TO_MESSAGE");
		}

		void SetUp() override {
			// Sets every setting a settings.toml would, so the fixture does not depend on test order.
			settings::install_test_defaults();
			world::set_calibration_config(world::CalibrationConfig{});
			world::set_calibration_override(world::Calibration::nominal());

			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
			settings::set_module_settings(module::ModuleId{TOWER_MODULE_ID}, settings::ModuleSettings{});

			settings::set_setting<std::string>("application_path", FAKE_CHILD_PATH);
			settings::set_setting("simulation_speed", 1.0);
			// Runtime mechanics tests use exact historical tick boundaries. Dedicated clock tests below
			// enable skew/loss explicitly; real scenario tests retain the physical defaults.
			settings::set_setting("module_start_offset_mean_ms", 0.0);
			settings::set_setting("module_start_offset_sd_ms", 0.0);
			settings::set_setting("module_clock_drift_sd_ppm", 0.0);
			settings::set_setting("module_tick_skip_mean_ppm", 0.0);
			settings::set_setting("module_tick_skip_sd_ppm", 0.0);

			// The runtime creates the world's modules from the layout, so only ids are set here.
			for (const uint8_t id : {FIRST, SECOND}) {
				settings::ModuleSettings module_settings{};
				module_settings.active = true;
				settings::set_module_settings(module::ModuleId{id}, module_settings);
			}

			clear_fake_child_environment();
		}

		void TearDown() override {
			world::clear_calibration_seed();
			world::set_calibration_config(world::CalibrationConfig{});
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
			clear_fake_child_environment();
		}

		// Grants the runtime a fixed amount of simulated time, one call per frame like the main loop.
		static void run_for(sim::Runtime& runtime, std::chrono::milliseconds simulated) {
			const int frames = static_cast<int>(simulated.count() / 16);
			for (int frame = 0; frame < frames; frame++) {
				runtime.advance(std::chrono::microseconds(16000), 1.0, std::chrono::milliseconds(50));
			}
		}
	};

} // namespace

TEST_F(RuntimeTest, StartsApplicationsAndWaitsForThemToActivate) {
	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	// Activation is the first heartbeat, so reaching this point means the whole loop ran.
	EXPECT_TRUE(runtime.platform().is_activated(module::ModuleId{FIRST}));
	EXPECT_TRUE(runtime.platform().is_activated(module::ModuleId{SECOND}));
	EXPECT_EQ(runtime.platform().lifecycle(module::ModuleId{FIRST}), platform::Lifecycle::RUNNING);
	EXPECT_GT(runtime.tick(), 0u);

	runtime.stop();
}

TEST_F(RuntimeTest, DeliversSynchronousRpcResponsesDuringBatchedActivation) {
	::setenv("FAKE_CHILD_STARTUP_RPC", "1", 1);

	sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, std::chrono::milliseconds(200), 2);
	ASSERT_NO_THROW(runtime.start());
	EXPECT_TRUE(runtime.platform().is_activated(module::ModuleId{FIRST}));
	EXPECT_TRUE(runtime.platform().is_activated(module::ModuleId{SECOND}));
	runtime.stop();
}

TEST_F(RuntimeTest, DropsAndAccountsForAShortMalformedMailboxFrame) {
	::setenv("FAKE_CHILD_INGRESS_PROBE_1", "1", 1);

	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());
	EXPECT_EQ(runtime.platform().rate_sample_count(module::ModuleId{FIRST}, platform::Bucket::GLOBAL),
			  runtime.platform().rate_sample_count(module::ModuleId{SECOND}, platform::Bucket::GLOBAL) + 1);
	runtime.stop();
}

TEST_F(RuntimeTest, DropsAndAccountsForAnUnexpectedResponseFrame) {
	::setenv("FAKE_CHILD_INGRESS_PROBE_1", "2", 1);

	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());
	EXPECT_EQ(runtime.platform().rate_sample_count(module::ModuleId{FIRST}, platform::Bucket::GLOBAL),
			  runtime.platform().rate_sample_count(module::ModuleId{SECOND}, platform::Bucket::GLOBAL) + 1);
	runtime.stop();
}

TEST_F(RuntimeTest, HeartbeatDeathStopsOnlyTheFailedModule) {
	::setenv("FAKE_CHILD_HEARTBEAT_UNTIL_1", "100", 1);

	sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, 5);
	ASSERT_NO_THROW(runtime.start());
	ASSERT_EQ(runtime.platform().lifecycle(module::ModuleId{FIRST}), platform::Lifecycle::RUNNING);
	ASSERT_EQ(runtime.platform().lifecycle(module::ModuleId{SECOND}), platform::Lifecycle::RUNNING);

	ASSERT_NO_THROW(runtime.advance(std::chrono::milliseconds(2200), 1.0, std::chrono::seconds(10)));
	ASSERT_EQ(runtime.platform().lifecycle(module::ModuleId{FIRST}), platform::Lifecycle::STOPPED);
	ASSERT_EQ(runtime.platform().lifecycle(module::ModuleId{SECOND}), platform::Lifecycle::RUNNING);
	EXPECT_FALSE(runtime.platform().is_activated(module::ModuleId{FIRST}));
	EXPECT_NE(runtime.platform().stop_reason(module::ModuleId{FIRST}).find("stopped sending heartbeats"),
			  std::string::npos);

	const uint64_t world_before  = runtime.tick();
	const uint64_t failed_before = runtime.module_tick(module::ModuleId{FIRST});
	const uint64_t live_before   = runtime.module_tick(module::ModuleId{SECOND});
	ASSERT_NO_THROW(runtime.advance(std::chrono::milliseconds(100), 1.0, std::chrono::seconds(5)));
	EXPECT_EQ(runtime.tick() - world_before, 100u);
	EXPECT_EQ(runtime.module_tick(module::ModuleId{FIRST}), failed_before);
	EXPECT_EQ(runtime.module_tick(module::ModuleId{SECOND}) - live_before, 100u);

	runtime.stop();
}

TEST_F(RuntimeTest, RateStopDeliversTryAgainBeforeRetiringOnlyTheOffendingModule) {
	const fs::path statuses = fs::temp_directory_path() / ("quercus_rate_stop_" + std::to_string(::getpid()));
	fs::remove(statuses);
	::setenv("FAKE_CHILD_RATE_BURST_1", "4", 1);
	::setenv("FAKE_CHILD_STATUS_RECORD", statuses.c_str(), 1);

	sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, sim::MAX_TICK_BATCH);
	ASSERT_NO_THROW(runtime.start());
	ASSERT_NO_THROW(runtime.advance(std::chrono::milliseconds(200), 1.0, std::chrono::seconds(10)));

	EXPECT_EQ(runtime.platform().lifecycle(module::ModuleId{FIRST}), platform::Lifecycle::STOPPED);
	EXPECT_EQ(runtime.platform().lifecycle(module::ModuleId{SECOND}), platform::Lifecycle::RUNNING);
	EXPECT_NE(runtime.platform().stop_reason(module::ModuleId{FIRST}).find("global bucket"), std::string::npos);

	std::ifstream input(statuses);
	std::string   line;
	bool          saw_terminal_try_again = false;
	while (std::getline(input, line)) {
		if (line.ends_with(":" + std::to_string(platform::STATUS_TRY_AGAIN))) saw_terminal_try_again = true;
	}
	EXPECT_TRUE(saw_terminal_try_again);

	const uint64_t world_before = runtime.tick();
	const uint64_t live_before  = runtime.module_tick(module::ModuleId{SECOND});
	ASSERT_NO_THROW(runtime.advance(std::chrono::milliseconds(100), 1.0, std::chrono::seconds(5)));
	EXPECT_EQ(runtime.tick() - world_before, 96u);
	EXPECT_EQ(runtime.module_tick(module::ModuleId{SECOND}) - live_before, 96u);

	runtime.stop();
	fs::remove(statuses);
}

TEST_F(RuntimeTest, AppliesEarlyScenarioEventsWhileStartupSkewAdvancesTime) {
	settings::set_setting("module_start_offset_mean_ms", 20.0);
	settings::set_setting("module_start_offset_sd_ms", 0.0);

	events::Schedule   schedule;
	events::TubArrival tub;
	tub.tick   = 1;
	tub.id     = 4;
	tub.module = module::ModuleId{FIRST};
	schedule.tubs.push_back(tub);

	sim::Runtime runtime(7);
	runtime.load(schedule);
	ASSERT_NO_THROW(runtime.start());
	EXPECT_GT(runtime.tick(), tub.tick);
	EXPECT_NE(runtime.world().tub(tub.id), nullptr);
	runtime.stop();
}

TEST_F(RuntimeTest, TracesScheduledAndYeetedTubRemovalsOnceWithTheirReason) {
	const fs::path trace_path =
		fs::temp_directory_path() / ("quercus_tub_removal_trace_" + std::to_string(::getpid()) + ".ndjson");
	fs::remove(trace_path);

	events::Schedule   schedule;
	events::TubArrival scheduled;
	scheduled.tick         = 0;
	scheduled.removed_tick = 2;
	scheduled.id           = 4;
	scheduled.module       = module::ModuleId{FIRST};
	schedule.tubs.push_back(scheduled);

	sim::Runtime runtime(7);
	runtime.load(schedule);
	runtime.world().place(5, module::ModuleId{FIRST}, world::Position::CENTER, RfidPayload{});
	runtime.world().module(module::ModuleId{FIRST})->set_servo_angle(module::ServoAngle{0});
	runtime.world().module(module::ModuleId{FIRST})->set_big_belt_speed(module::BeltSpeed{100});
	runtime.trace_to(trace_path.string(), {});
	ASSERT_NO_THROW(runtime.start());
	ASSERT_NO_THROW(runtime.advance(std::chrono::milliseconds(3000), 1.0, std::chrono::seconds(10)));
	EXPECT_EQ(runtime.world().tub(4), nullptr);
	EXPECT_EQ(runtime.world().tub(5), nullptr);
	runtime.stop();

	const world::Calibration calibration = world::Calibration::nominal();
	int64_t                  position_um  = calibration.endpoint_to_junction_um;
	int64_t                  carry        = 0;
	uint64_t                 travel_ticks = 0;
	while (position_um < 2 * calibration.endpoint_to_junction_um) {
		position_um += world::belt_step_um(100.0, calibration.mechanical.big_belt, carry);
		travel_ticks++;
	}
	const uint64_t yeeted_tick = travel_ticks + world::TUB_OOB_YEET_MS - 1;

	std::ifstream      input(trace_path);
	std::ostringstream trace;
	trace << input.rdbuf();
	fs::remove(trace_path);
	const std::string contents        = trace.str();
	const std::string scheduled_event = "\"type\":\"tub_removed\",\"tick\":2,\"id\":4,\"reason\":\"scheduled\"";
	const std::string yeeted_event =
		"\"type\":\"tub_removed\",\"tick\":" + std::to_string(yeeted_tick) + ",\"id\":5,\"reason\":\"yeeted\"";
	const std::size_t scheduled_at = contents.find(scheduled_event);
	const std::size_t yeeted_at    = contents.find(yeeted_event);
	ASSERT_NE(scheduled_at, std::string::npos);
	ASSERT_NE(yeeted_at, std::string::npos);
	EXPECT_EQ(contents.find(scheduled_event, scheduled_at + 1), std::string::npos);
	EXPECT_EQ(contents.find(yeeted_event, yeeted_at + 1), std::string::npos);
}

TEST_F(RuntimeTest, RejectsScenariosLoadedAfterLogicalTimeStarts) {
	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	events::Schedule schedule;
	EXPECT_THROW(runtime.load(schedule), std::logic_error);
	runtime.stop();
}

TEST_F(RuntimeTest, SkippedModuleTicksRemainVisibleToGlobalLtlTime) {
	settings::set_setting("module_start_offset_mean_ms", 0.0);
	settings::set_setting("module_start_offset_sd_ms", 0.0);
	settings::set_setting("module_tick_skip_mean_ppm", 1'000'000.0);
	settings::set_setting("module_tick_skip_sd_ppm", 0.0);

	ltl::File properties = ltl::parse(R"(
let m = module(1);
prop frozen_module_emits_nothing =
	always ((count request in rpc_requests(m) where (request.module == m)) == 0);
prop global_time_still_reaches_the_end at 3ms = true;
)",
									  "clock-skips.spec");

	const fs::path trace_path =
		fs::temp_directory_path() / ("quercus_tick_skip_" + std::to_string(::getpid()) + ".ndjson");
	fs::remove(trace_path);
	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 3, "clock-skips.spec");
	runtime.trace_to(trace_path.string(), {});
	ASSERT_NO_THROW(runtime.start(3));
	EXPECT_EQ(runtime.tick(), 4u);
	EXPECT_EQ(runtime.module_tick(module::ModuleId{FIRST}), 0u);
	EXPECT_EQ(runtime.module_tick(module::ModuleId{SECOND}), 0u);
	EXPECT_FALSE(runtime.platform().is_activated(module::ModuleId{FIRST}));
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::TRUE);
	EXPECT_EQ(results[1].verdict, ltl::Verdict::TRUE);
	runtime.stop();

	std::ifstream      input(trace_path);
	std::ostringstream trace;
	trace << input.rdbuf();
	fs::remove(trace_path);
	EXPECT_NE(trace.str().find("\"type\":\"module_tick_skipped\",\"tick\":0,\"module\":1"), std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"property_verdict\""), std::string::npos);
}

TEST_F(RuntimeTest, RecordsSampledModuleClockStateAndStartupActivity) {
	settings::set_setting("module_start_offset_mean_ms", 4.0);
	settings::set_setting("module_start_offset_sd_ms", 0.0);
	settings::set_setting("module_clock_drift_sd_ppm", 0.0);
	settings::set_setting("module_tick_skip_mean_ppm", 1'000'000.0);
	settings::set_setting("module_tick_skip_sd_ppm", 0.0);

	const fs::path trace_path =
		fs::temp_directory_path() / ("quercus_startup_trace_" + std::to_string(::getpid()) + ".ndjson");
	fs::remove(trace_path);
	sim::Runtime runtime(7);
	runtime.trace_to(trace_path.string(), {});
	ASSERT_NO_THROW(runtime.start(4));
	runtime.stop();

	std::ifstream      input(trace_path);
	std::ostringstream trace;
	trace << input.rdbuf();
	fs::remove(trace_path);
	EXPECT_NE(trace.str().find(
				  "\"type\":\"module_clock\",\"module\":1,\"start_tick\":4,\"rate_ppm\":1000000,\"skip_ppm\":1000000"),
			  std::string::npos);
	EXPECT_NE(trace.str().find(
				  "\"type\":\"module_clock\",\"module\":2,\"start_tick\":4,\"rate_ppm\":1000000,\"skip_ppm\":1000000"),
			  std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"module_tick_skipped\",\"tick\":4,\"module\":1"), std::string::npos);
}

TEST_F(RuntimeTest, AdvancesSimulatedTimeInProportionToElapsedTime) {
	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	const uint64_t started = runtime.tick();
	run_for(runtime, std::chrono::milliseconds(160));

	// Up to 160 ticks; less if the children could not keep up inside the per-frame budget.
	const uint64_t advanced = runtime.tick() - started;
	EXPECT_GT(advanced, 0u);
	EXPECT_LE(advanced, 160u);

	runtime.stop();
}

TEST_F(RuntimeTest, UsesTheTimeDeadlineRatherThanAnArbitraryTickLimit) {
	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	const uint64_t started = runtime.tick();
	runtime.advance(std::chrono::milliseconds(300), 1.0, std::chrono::seconds(5));

	EXPECT_EQ(runtime.tick() - started, 300u);

	runtime.stop();
}

TEST_F(RuntimeTest, ReplaysEveryTickOfABatch) {
	sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, 3);
	ASSERT_NO_THROW(runtime.start());

	const uint64_t started        = runtime.tick();
	const uint64_t first_started  = runtime.module_tick(module::ModuleId{FIRST});
	const uint64_t second_started = runtime.module_tick(module::ModuleId{SECOND});
	runtime.advance(std::chrono::milliseconds(300), 1.0, std::chrono::seconds(5));
	EXPECT_EQ(runtime.tick() - started, 300u);
	EXPECT_EQ(runtime.module_tick(module::ModuleId{FIRST}) - first_started, 300u);
	EXPECT_EQ(runtime.module_tick(module::ModuleId{SECOND}) - second_started, 300u);

	runtime.stop();
}

TEST_F(RuntimeTest, GrantsMultipleTicksInOneBarrierWhenClockSkipsAreDisabled) {
	const fs::path record = fs::temp_directory_path() / ("quercus_batch_grants_" + std::to_string(::getpid()));
	fs::remove(record);
	::setenv("FAKE_CHILD_GRANT_RECORD", record.c_str(), 1);

	sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, 3);
	ASSERT_NO_THROW(runtime.start());
	fs::remove(record);
	runtime.advance(std::chrono::milliseconds(3), 1.0, std::chrono::seconds(5));
	runtime.stop();

	std::ifstream file(record);
	std::string   grant;
	ASSERT_TRUE(std::getline(file, grant));
	EXPECT_EQ(grant, "3:7");
	EXPECT_FALSE(std::getline(file, grant));
	fs::remove(record);
}

TEST_F(RuntimeTest, UsesRequestedBatchWhenClockSkipsAreEnabled) {
	settings::set_setting("module_tick_skip_mean_ppm", 500'000.0);
	settings::set_setting("module_tick_skip_sd_ppm", 0.0);
	const fs::path trace_path =
		fs::temp_directory_path() / ("quercus_effective_batch_" + std::to_string(::getpid()) + ".ndjson");
	const fs::path grant_path = fs::temp_directory_path() / ("quercus_skip_batch_grants_" + std::to_string(::getpid()));
	fs::remove(trace_path);
	fs::remove(grant_path);
	::setenv("FAKE_CHILD_GRANT_RECORD", grant_path.c_str(), 1);

	sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, 5);
	runtime.trace_to(trace_path.string(), {});
	testing::internal::CaptureStderr();
	ASSERT_NO_THROW(runtime.start());
	const std::string warning = testing::internal::GetCapturedStderr();
	fs::remove(grant_path);
	runtime.advance(std::chrono::milliseconds(5), 1.0, std::chrono::seconds(5));
	runtime.stop();

	std::ifstream      input(trace_path);
	std::ostringstream trace;
	trace << input.rdbuf();
	fs::remove(trace_path);
	EXPECT_EQ(warning.find("effective tick batch is 1"), std::string::npos);
	EXPECT_NE(trace.str().find("\"tick_batch_requested\":5,\"tick_batch_effective\":5"), std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"module_tick_skipped\""), std::string::npos);

	std::ifstream grant_input(grant_path);
	std::string   grant;
	ASSERT_TRUE(std::getline(grant_input, grant));
	EXPECT_EQ(grant.substr(0, 2), "5:");
	EXPECT_NE(grant, "5:31");
	fs::remove(grant_path);
}

TEST_F(RuntimeTest, TickSkippingProducesTheSameTraceAcrossBatchSizes) {
	settings::set_setting("module_tick_skip_mean_ppm", 300'000.0);
	settings::set_setting("module_tick_skip_sd_ppm", 0.0);
	settings::set_setting("module_clock_drift_sd_ppm", 137.0);
	::setenv("FAKE_CHILD_HEARTBEAT_EVERY", "4294967295", 1);

	const auto execute = [](uint32_t batch) {
		const fs::path path = fs::temp_directory_path() / ("quercus_skip_trace_" + std::to_string(::getpid()) + "_" +
														   std::to_string(batch) + ".ndjson");
		fs::remove(path);
		sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, batch);
		runtime.start();
		// Begin after activation so the comparison isolates skipped-tick batching from the documented
		// batch-boundary delay of responses to startup heartbeats.
		runtime.trace_to(path.string(), {});
		const uint64_t before = runtime.tick();
		runtime.advance(std::chrono::milliseconds(300), 1.0, std::chrono::seconds(10));
		EXPECT_EQ(runtime.tick() - before, 300u);
		runtime.stop();

		std::ifstream      input(path);
		std::ostringstream trace;
		trace << input.rdbuf();
		fs::remove(path);
		const std::string complete  = trace.str();
		const std::size_t first_end = complete.find('\n');
		EXPECT_NE(first_end, std::string::npos);
		return first_end == std::string::npos ? std::string{} : complete.substr(first_end + 1);
	};

	const std::string strict   = execute(1);
	const std::string batch_5  = execute(5);
	const std::string batch_15 = execute(15);
	EXPECT_FALSE(strict.empty());
	EXPECT_EQ(batch_5, strict);
	EXPECT_EQ(batch_15, strict);
}

TEST_F(RuntimeTest, RpcServiceDelaysCountExecutedModuleTicks) {
	settings::set_setting("module_tick_skip_mean_ppm", 900'000.0);
	settings::set_setting("module_tick_skip_sd_ppm", 0.0);
	::setenv("FAKE_CHILD_QUERY_TICK", "2", 1);

	const fs::path trace_path =
		fs::temp_directory_path() / ("quercus_local_rpc_delay_" + std::to_string(::getpid()) + ".ndjson");
	fs::remove(trace_path);
	sim::Runtime runtime(7);
	runtime.trace_to(trace_path.string(), {});
	ASSERT_NO_THROW(runtime.start());
	runtime.advance(std::chrono::milliseconds(1000), 1.0, std::chrono::seconds(10));
	runtime.stop();

	std::ifstream           trace(trace_path);
	std::string             line;
	std::optional<uint64_t> request_tick;
	std::optional<uint64_t> response_tick;
	const auto              read_tick = [](const std::string& record) {
		const std::string marker = "\"tick\":";
		const std::size_t begin  = record.find(marker);
		return std::stoull(record.substr(begin + marker.size()));
	};
	while (std::getline(trace, line)) {
		if (line.find("\"type\":\"rpc_request\"") != std::string::npos &&
			line.find("\"module\":1,\"cmd\":" + std::to_string(CMD_LASER_LEFT_GET)) != std::string::npos) {
			request_tick = read_tick(line);
		}
		if (line.find("\"type\":\"rpc_response\"") != std::string::npos &&
			line.find("\"module\":1,\"cmd\":" + std::to_string(CMD_LASER_LEFT_GET)) != std::string::npos) {
			response_tick = read_tick(line);
		}
	}
	fs::remove(trace_path);
	ASSERT_TRUE(request_tick.has_value());
	ASSERT_TRUE(response_tick.has_value());
	EXPECT_GT(*response_tick, *request_tick + sim::RPC_RX_DELAY_LOCAL_TICKS + sim::RPC_TX_DELAY_LOCAL_TICKS)
		<< "90% skipped module ticks must stretch local RPC service delays in global time";
}

TEST_F(RuntimeTest, LargerBatchesReduceLockstepRuntimeByAtLeastTwentyPercentEach) {
	settings::set_setting("module_tick_skip_mean_ppm", 1000.0);
	settings::set_setting("module_tick_skip_sd_ppm", 100.0);
	settings::set_setting("module_clock_drift_sd_ppm", 100.0);
	::setenv("FAKE_CHILD_HEARTBEAT_EVERY", "100", 1);

	const auto measure = [](uint32_t batch) {
		sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, batch);
		runtime.start();
		runtime.advance(std::chrono::milliseconds(300), 1.0, std::chrono::seconds(10));
		const uint64_t before_tick = runtime.tick();
		const auto     before_time = std::chrono::steady_clock::now();
		runtime.advance(std::chrono::milliseconds(6000), 1.0, std::chrono::seconds(30));
		const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - before_time).count();
		EXPECT_EQ(runtime.tick() - before_tick, 6000u);
		runtime.stop();
		return elapsed;
	};
	const auto fastest_of_three = [&measure](uint32_t batch) {
		return std::min({measure(batch), measure(batch), measure(batch)});
	};

	const double strict   = fastest_of_three(1);
	const double batch_5  = fastest_of_three(5);
	const double batch_15 = fastest_of_three(15);
	RecordProperty("batch_1_seconds", strict);
	RecordProperty("batch_5_seconds", batch_5);
	RecordProperty("batch_15_seconds", batch_15);
	EXPECT_LT(batch_5, strict * 0.8) << "1x=" << strict << "s, 5x=" << batch_5 << "s";
	EXPECT_LT(batch_15, batch_5 * 0.8) << "5x=" << batch_5 << "s, 15x=" << batch_15 << "s";
}

TEST_F(RuntimeTest, RetainsTheLastInboundSlotAcrossAdvanceCalls) {
	const fs::path record = fs::temp_directory_path() / ("quercus_child_record_" + std::to_string(::getpid()));
	fs::remove(record);
	::setenv("FAKE_CHILD_QUERY_TICK", "2", 1);
	::setenv("FAKE_CHILD_RECORD", record.c_str(), 1);

	sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, 3);
	ASSERT_NO_THROW(runtime.start());

	// The child sends the query from relative slot zero. Its response is produced in slot two while
	// the parent replays the first batch, then must survive a one-tick budget tail until the next
	// complete batch consumes it at its relative slot two.
	runtime.advance(std::chrono::milliseconds(3), 1.0, std::chrono::seconds(5));
	runtime.advance(std::chrono::milliseconds(1), 1.0, std::chrono::seconds(5));
	runtime.advance(std::chrono::milliseconds(2), 1.0, std::chrono::seconds(5));
	runtime.stop();

	std::ifstream file(record);
	std::string   line;
	bool          saw_response = false;
	while (std::getline(file, line)) {
		if (line == std::to_string(MT_RESP) + ":" + std::to_string(CMD_LASER_LEFT_GET)) {
			saw_response = true;
		}
	}
	EXPECT_TRUE(saw_response);
	fs::remove(record);
}

TEST_F(RuntimeTest, WarnsOnlyAfterTenFramesOfSustainedPacingShortfall) {
	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	testing::internal::CaptureStderr();
	for (int frame = 0; frame < 9; frame++) {
		runtime.advance(std::chrono::milliseconds(100), 100.0, std::chrono::microseconds(1));
	}
	EXPECT_TRUE(testing::internal::GetCapturedStderr().empty());

	testing::internal::CaptureStderr();
	runtime.advance(std::chrono::milliseconds(100), 100.0, std::chrono::microseconds(1));
	EXPECT_NE(testing::internal::GetCapturedStderr().find("at least 5% below"), std::string::npos);

	// A continuing shortfall does not spam the same warning every frame.
	testing::internal::CaptureStderr();
	runtime.advance(std::chrono::milliseconds(100), 100.0, std::chrono::microseconds(1));
	EXPECT_TRUE(testing::internal::GetCapturedStderr().empty());

	runtime.stop();
}

TEST_F(RuntimeTest, GrantsNoTicksWhilePaused) {
	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	const uint64_t started = runtime.tick();
	for (int frame = 0; frame < 10; frame++) {
		runtime.advance(std::chrono::microseconds(16000), 0.0, std::chrono::milliseconds(50));
	}

	EXPECT_EQ(runtime.tick(), started);

	runtime.stop();
}

TEST_F(RuntimeTest, DeliversEventsToASubscribedApplication) {
	const fs::path record = fs::temp_directory_path() / ("quercus_child_record_" + std::to_string(::getpid()));
	fs::remove(record);
	::setenv("FAKE_CHILD_SUBSCRIBE", std::to_string(EVENT_RFID_DETECT).c_str(), 1);
	::setenv("FAKE_CHILD_RECORD", record.c_str(), 1);

	events::Schedule schedule;

	events::TubArrival tub;
	tub.tick   = 10;
	tub.id     = 4;
	tub.module = module::ModuleId{FIRST};
	schedule.tubs.push_back(tub);

	sim::Runtime runtime(7);
	runtime.load(schedule);
	ltl::File properties = ltl::parse(R"(
prop observes_the_rfid_delivery =
    eventually (
        exists event in rpc_events(module(1)):
            event.kind == .rpc_event_rfid_detect
    );
prop never_delivers_duplicate_rfid_events_on_one_tick =
    always (
        count event in rpc_events(module(1)) where (
            event.kind == .rpc_event_rfid_detect
        ) <= 1
    );
)",
									  "rfid-events.spec");
	runtime.load_properties(std::move(properties), 100, "rfid-events.spec");
	ASSERT_NO_THROW(runtime.start(100));
	while (runtime.properties_pending() > 0) {
		runtime.advance(std::chrono::milliseconds(16), 1.0, std::chrono::seconds(5));
	}
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::TRUE);
	EXPECT_EQ(results[1].verdict, ltl::Verdict::TRUE);

	runtime.stop();

	std::ifstream file(record);
	std::string   line;
	bool          saw_event = false;
	while (std::getline(file, line)) {
		if (line == std::to_string(MT_EVENT) + ":" + std::to_string(EVENT_RFID_DETECT)) {
			saw_event = true;
		}
	}
	EXPECT_TRUE(saw_event);

	fs::remove(record);
}

TEST_F(RuntimeTest, HardwareEventsIncludeConfiguredDelayAndNextTickHandoff) {
	const fs::path record = fs::temp_directory_path() / ("quercus_hardware_event_ticks_" + std::to_string(::getpid()));
	fs::remove(record);
	::setenv("FAKE_CHILD_SUBSCRIBE", std::to_string(EVENT_LASER_LEFT_DETECT | EVENT_RFID_DETECT).c_str(), 1);
	::setenv("FAKE_CHILD_RECORD", record.c_str(), 1);
	::setenv("FAKE_CHILD_RECORD_TICKS", "1", 1);
	settings::set_setting("laser_event_extra_delay_ms", 3);
	settings::set_setting("rfid_event_extra_delay_mean_ms", 35.0);
	settings::set_setting("rfid_event_extra_delay_sd_ms", 0.0);

	events::Schedule   schedule;
	events::TubArrival laser;
	laser.tick     = 10;
	laser.id       = 4;
	laser.module   = module::ModuleId{FIRST};
	laser.position = world::Position::LEFT_LASER;
	schedule.tubs.push_back(laser);

	events::TubArrival rfid;
	rfid.tick   = 20;
	rfid.id     = 5;
	rfid.module = module::ModuleId{FIRST};
	rfid.uid    = {1, 2, 3, 4, 5, 6, 7};
	schedule.tubs.push_back(rfid);

	sim::Runtime runtime(7);
	runtime.load(schedule);
	ASSERT_NO_THROW(runtime.start());
	runtime.world().module(module::ModuleId{FIRST})->set_left_laser_enabled(true);
	runtime.advance(std::chrono::milliseconds(64), 1.0, std::chrono::seconds(5));
	runtime.stop();

	std::ifstream file(record);
	std::string   line;
	bool          saw_laser_at_14 = false;
	bool          saw_rfid_at_56  = false;
	while (std::getline(file, line)) {
		if (line == "14:" + std::to_string(MT_EVENT) + ":" + std::to_string(EVENT_LASER_LEFT_DETECT)) {
			saw_laser_at_14 = true;
		}
		if (line == "56:" + std::to_string(MT_EVENT) + ":" + std::to_string(EVENT_RFID_DETECT)) {
			saw_rfid_at_56 = true;
		}
	}
	EXPECT_TRUE(saw_laser_at_14) << "3 ms laser latency plus the 1 ms handoff must total 4 ms";
	EXPECT_TRUE(saw_rfid_at_56) << "35 ms RFID latency plus the 1 ms handoff must total 36 ms";

	fs::remove(record);
}

TEST_F(RuntimeTest, FailsTheRunWhenAnApplicationExits) {
	::setenv("FAKE_CHILD_EXIT_AFTER", "5", 1);

	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	// Carrying on would produce a run the inputs no longer describe, so the run ends instead.
	EXPECT_THROW(run_for(runtime, std::chrono::milliseconds(160)), std::runtime_error);

	runtime.stop();
}

TEST_F(RuntimeTest, WaitsForEveryApplicationBeforeCompletingATick) {
	sim::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	// The barrier keeps every application on the same tick, which is what makes a run reproducible.
	for (int frame = 0; frame < 5; frame++) {
		const uint64_t before = runtime.tick();
		runtime.advance(std::chrono::microseconds(16000), 1.0, std::chrono::milliseconds(200));
		EXPECT_GT(runtime.tick(), before);
	}

	runtime.stop();
}

TEST_F(RuntimeTest, FailsWhenAnApplicationCannotBeStarted) {
	settings::set_setting<std::string>("application_path", "/nonexistent/quercus/application");

	sim::Runtime runtime(7, std::chrono::milliseconds(200), std::chrono::milliseconds(200));
	EXPECT_THROW(runtime.start(), std::runtime_error);
}

TEST_F(RuntimeTest, TreatsAnEmptyPropertyFileAsAVacuousPass) {
	ltl::File properties = ltl::parse("", "empty.spec");

	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 0, "empty.spec");
	ASSERT_NO_THROW(runtime.start());
	EXPECT_EQ(runtime.properties_total(), 0u);
	EXPECT_EQ(runtime.properties_pending(), 0u);
	EXPECT_FALSE(runtime.failed());
	runtime.stop();
}

TEST_F(RuntimeTest, EvaluatesPropertyScopesDirectlyFromCanonicalObservations) {
	events::Schedule   schedule;
	events::TubArrival tub;
	tub.tick         = 0;
	tub.removed_tick = 2;
	tub.id           = 4;
	tub.module       = module::ModuleId{FIRST};
	schedule.tubs.push_back(tub);

	ltl::File properties = ltl::parse(R"(
prop present_at_start at 0ms = present(tub(4));
prop removed_by_endpoint during [1ms, 2ms] = eventually disappeared(tub(4));
prop whole_trace = always true;
)",
									  "runtime.spec");

	sim::Runtime runtime(7);
	runtime.load(schedule);
	runtime.load_properties(std::move(properties), 2, "runtime.spec");
	EXPECT_EQ(runtime.properties_total(), 3u);
	ASSERT_NO_THROW(runtime.start());

	run_for(runtime, std::chrono::milliseconds(16));
	EXPECT_FALSE(runtime.failed());
	EXPECT_EQ(runtime.properties_pending(), 0u);
	runtime.stop();
}

TEST_F(RuntimeTest, DoesNotDecideAConstantPropertyBeforeItsScopeBegins) {
	ltl::File properties = ltl::parse(R"(
prop future_truth at 100ms = true;
prop future_falsehood at 100ms = false;
)",
									  "runtime.spec");

	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 100, "runtime.spec");
	EXPECT_EQ(runtime.properties_pending(), 2u);
	EXPECT_EQ(runtime.properties_failed(), 0u);
	EXPECT_FALSE(runtime.failed());
	const std::vector<sim::PropertyResult> initial_results = runtime.property_results();
	ASSERT_EQ(initial_results.size(), 2u);
	EXPECT_EQ(initial_results[0].name, "future_truth");
	EXPECT_EQ(initial_results[0].verdict, ltl::Verdict::PENDING);
	EXPECT_FALSE(initial_results[0].decisive_tick.has_value());

	ASSERT_NO_THROW(runtime.start());
	ASSERT_LT(runtime.tick(), 101u);
	run_for(runtime, std::chrono::milliseconds(96));
	ASSERT_LT(runtime.tick(), 101u);
	EXPECT_EQ(runtime.properties_pending(), 2u);
	EXPECT_FALSE(runtime.failed());

	run_for(runtime, std::chrono::milliseconds(16));
	EXPECT_EQ(runtime.tick(), 101u);
	EXPECT_EQ(runtime.properties_pending(), 0u);
	EXPECT_EQ(runtime.properties_failed(), 1u);
	EXPECT_TRUE(runtime.failed());
	const std::vector<sim::PropertyResult> results = runtime.property_results();
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::TRUE);
	EXPECT_EQ(results[0].decisive_tick, 100u);
	EXPECT_EQ(results[1].name, "future_falsehood");
	EXPECT_EQ(results[1].verdict, ltl::Verdict::FALSE);
	EXPECT_EQ(results[1].decisive_tick, 100u);
	runtime.stop();
}

TEST_F(RuntimeTest, StopsStartupAtAnInclusiveExecutionLimit) {
	ltl::File properties = ltl::parse("prop future during [0ms, 100ms] = eventually false;", "runtime.spec");

	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 100, "runtime.spec");
	ASSERT_NO_THROW(runtime.start(0));
	EXPECT_EQ(runtime.tick(), 1u);
	EXPECT_EQ(runtime.properties_pending(), 1u);
	EXPECT_FALSE(runtime.failed());
	runtime.stop();
}

TEST_F(RuntimeTest, LeavesIncompleteAbsoluteScopesPendingAtTheScenarioEnd) {
	ltl::File properties = ltl::parse(R"(
prop incomplete during [1ms, 4ms] = always true;
prop unavailable at 4ms = true;
prop resolved_early during [1ms, 4ms] = eventually true;
)",
									  "runtime.spec");

	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 2, "runtime.spec");
	ASSERT_NO_THROW(runtime.start(2));
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 3u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::PENDING);
	EXPECT_EQ(results[1].verdict, ltl::Verdict::PENDING);
	EXPECT_EQ(results[2].verdict, ltl::Verdict::TRUE);
	EXPECT_EQ(results[2].decisive_tick, 1u);
	EXPECT_EQ(runtime.properties_pending(), 2u);
	EXPECT_FALSE(runtime.failed());
	runtime.stop();
}

TEST_F(RuntimeTest, DoesNotExtendPropertyObservationsPastTheSemanticScenarioEnd) {
	ltl::File properties = ltl::parse("prop unavailable at 4ms = true;", "runtime.spec");

	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 2, "runtime.spec");
	ASSERT_NO_THROW(runtime.start(5));
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::PENDING);
	EXPECT_FALSE(results[0].decisive_tick.has_value());
	runtime.stop();
}

TEST_F(RuntimeTest, MetricBoundsAreRelativeToANonzeroDuringScopeStart) {
	ltl::File properties = ltl::parse(R"(
prop relative during [3ms, 8ms] = eventually[1ms, 2ms] module(1).led_color == 0x000000;
)",
									  "runtime.spec");

	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 8, "runtime.spec");
	ASSERT_NO_THROW(runtime.start(8));
	while (runtime.properties_pending() > 0 && runtime.tick() <= 8) {
		runtime.advance(std::chrono::milliseconds(16), 1.0, std::chrono::seconds(5));
	}
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::TRUE);
	EXPECT_EQ(results[0].decisive_tick, 4u) << "offset 1 is measured from the absolute during-scope start at tick 3";
	runtime.stop();
}

TEST_F(RuntimeTest, StopsWhenAPropertyFails) {
	ltl::File properties =
		ltl::parse("prop impossible at 1ms = module(1).rfid_active && !module(1).rfid_active;", "runtime.spec");

	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 2, "runtime.spec");
	ASSERT_NO_THROW(runtime.start());
	run_for(runtime, std::chrono::milliseconds(16));
	ASSERT_TRUE(runtime.failed());

	const uint64_t stopped = runtime.tick();
	run_for(runtime, std::chrono::milliseconds(16));
	EXPECT_EQ(runtime.tick(), stopped);
	runtime.stop();
}

TEST_F(RuntimeTest, ObservesStartupTubEdgesAtLogicalTimeZero) {
	events::Schedule   schedule;
	events::TubArrival tub;
	tub.tick   = 0;
	tub.id     = 4;
	tub.module = module::ModuleId{FIRST};
	schedule.tubs.push_back(tub);

	ltl::File properties =
		ltl::parse("prop startup at 0ms = appeared(tub(4)) && entered(tub(4), module(1).entry);", "startup.spec");
	sim::Runtime runtime(7);
	runtime.load(schedule);
	runtime.load_properties(std::move(properties), 0, "startup.spec");
	ASSERT_NO_THROW(runtime.start(0));

	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::TRUE);
	EXPECT_EQ(results[0].decisive_tick, 0u);
	runtime.stop();
}

TEST_F(RuntimeTest, RecordsFailureFormulaObligationFactsAndCanonicalObservation) {
	const fs::path trace_path =
		fs::temp_directory_path() / ("ltlf_failure_trace_" + std::to_string(::getpid()) + ".ndjson");
	fs::remove(trace_path);

	events::Schedule   schedule;
	events::TubArrival tub;
	tub.tick   = 0;
	tub.id     = 4;
	tub.module = module::ModuleId{FIRST};
	schedule.tubs.push_back(tub);

	ltl::File    properties = ltl::parse("prop broken at 0ms = !present(tub(4));", "failure.spec");
	sim::Runtime runtime(7);
	runtime.load(schedule);
	runtime.load_properties(std::move(properties), 0, "failure.spec");
	runtime.trace_to(trace_path.string(), {});
	ASSERT_NO_THROW(runtime.start(0));

	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 1u);
	ASSERT_TRUE(results[0].failure.has_value());
	EXPECT_EQ(results[0].failure->formula, "!(present(tub(4)))");
	ASSERT_EQ(results[0].failure->facts.size(), 1u);
	EXPECT_EQ(results[0].failure->facts[0].expression, "present(tub(4))");
	EXPECT_TRUE(results[0].failure->facts[0].value);
	runtime.stop();

	std::ifstream      input(trace_path);
	std::ostringstream trace;
	trace << input.rdbuf();
	fs::remove(trace_path);
	EXPECT_NE(trace.str().find("\"type\":\"property_activation\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"property_verdict\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"verdict\":\"false\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"property_fact\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"tub\",\"tick\":0,\"id\":4"), std::string::npos);
}

TEST_F(RuntimeTest, RecordsMetricObligationCreationAndDeadline) {
	const fs::path trace_path =
		fs::temp_directory_path() / ("ltlf_deadline_trace_" + std::to_string(::getpid()) + ".ndjson");
	fs::remove(trace_path);

	ltl::File properties =
		ltl::parse("prop deadline = eventually[1ms, 2ms] module(1).led_color == 0xffffff;", "deadline.spec");
	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 4, "deadline.spec");
	runtime.trace_to(trace_path.string(), {});
	ASSERT_NO_THROW(runtime.start(4));
	while (!runtime.failed() && runtime.tick() <= 4) {
		runtime.advance(std::chrono::milliseconds(16), 1.0, std::chrono::seconds(5));
	}

	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::FALSE);
	EXPECT_EQ(results[0].decisive_tick, 2u);
	ASSERT_TRUE(results[0].failure.has_value());
	EXPECT_EQ(results[0].failure->obligation_created_tick, 0u);
	EXPECT_EQ(results[0].failure->deadline_tick, 2u);
	runtime.stop();

	std::ifstream      input(trace_path);
	std::ostringstream trace;
	trace << input.rdbuf();
	fs::remove(trace_path);
	EXPECT_NE(trace.str().find("\"ltlf_version\":\"0.3\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"obligation_created_tick\":0"), std::string::npos);
	EXPECT_NE(trace.str().find("\"deadline_tick\":2"), std::string::npos);
	EXPECT_NE(trace.str().find("\"actual\":\"0x000000\""), std::string::npos);
}

TEST_F(RuntimeTest, ObservesRpcTrafficAtExactBatchedLogicalBoundaries) {
	const fs::path trace_path =
		fs::temp_directory_path() / ("ltlf_rpc_trace_" + std::to_string(::getpid()) + ".ndjson");
	fs::remove(trace_path);
	::setenv("FAKE_CHILD_QUERY_TICK", "2", 1);

	ltl::File properties = ltl::parse(R"(
prop every_query_receives_a_successful_response =
    always (
        forall request in rpc_requests(module(1)):
            (request.cmd == .rpc_cmd_laser_left_get)
            -> (eventually[0ms, 5ms] (rpc_response_to(request).status == .ok))
    );
prop every_delivered_response_is_correlated_and_successful =
    always (
        forall response in rpc_responses(module(1)):
            (response.request.id == response.id) && (response.status == .ok)
    );
)",
									  "rpc-boundary.spec");

	sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, 3);
	runtime.load_properties(std::move(properties), 8, "rpc-boundary.spec");
	runtime.trace_to(trace_path.string(), {});
	ASSERT_NO_THROW(runtime.start(8));
	while (runtime.properties_pending() > 0) {
		runtime.advance(std::chrono::milliseconds(48), 1.0, std::chrono::seconds(5));
	}

	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::TRUE);
	EXPECT_EQ(results[1].verdict, ltl::Verdict::TRUE);
	runtime.stop();

	std::ifstream      input(trace_path);
	std::ostringstream trace;
	trace << input.rdbuf();
	fs::remove(trace_path);
	const std::string records = trace.str();

	// The request is replayed at tick two. Its response becomes ready while the parent replays the
	// rest of that child grant, is observed at the tick-four boundary, and is consumed on the child's
	// first executable tick in the following window.
	const std::string request =
		"\"type\":\"rpc_request\",\"tick\":2,\"id\":3,\"module\":1,\"cmd\":" + std::to_string(CMD_LASER_LEFT_GET);
	const std::string response =
		"\"type\":\"rpc_response\",\"tick\":4,\"id\":3,\"request_id\":3,\"module\":1,\"cmd\":" +
		std::to_string(CMD_LASER_LEFT_GET);
	EXPECT_NE(records.find(request), std::string::npos);
	EXPECT_NE(records.find(response), std::string::npos) << records;
}

TEST_F(RuntimeTest, MessageDeliveryProducesThePayloadCheckedReplyOnExactlyTheNextTick) {
	::setenv("FAKE_CHILD_REPLY_TO_MESSAGE", "1", 1);
	ltl::File properties = ltl::parse(R"(
prop replies_on_the_next_tick =
    always (
        forall event in rpc_events(module(1)):
            (event.kind == .rpc_event_message_received)
            -> (next (
                count request in rpc_requests(module(1)) where (
                    request.cmd == .rpc_cmd_send_packet
                    && request.payload == "\x02reply"
                ) >= 1
            ))
    );
)",
									  "message-reply.spec");

	sim::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 10, "message-reply.spec");
	ASSERT_NO_THROW(runtime.start(10));
	rpc::Frame message;
	message.type    = MT_EVENT;
	message.cmd     = EVENT_MESSAGE_RECEIVED;
	message.payload = {'i', 'n'};
	runtime.platform().enqueue(module::ModuleId{FIRST}, std::move(message));

	while (runtime.properties_pending() > 0) {
		runtime.advance(std::chrono::milliseconds(16), 1.0, std::chrono::seconds(5));
	}
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::TRUE);
	runtime.stop();
}

TEST_F(RuntimeTest, ControlledApplicationMutantsFailTheirIntendedProperties) {
	struct MutantCase {
		const char*  mode;
		const char*  formula;
		bool         should_fail_early;
		ltl::Verdict expected;
	};

	// A mutant caught inside the trace is refuted. `omit_leg` is only caught by the trace running
	// out, which is undecided rather than refuted: the belt this mutant never drives could still
	// have moved later. Either way the property does not hold, so the mutant is still detected.
	const MutantCase mutants[] = {
		{"reverse_leg", "always (module(1).big_belt_speed >= 0.0)", true, ltl::Verdict::FALSE},
		{"wrong_laser", "(always !module(1).right_laser_enabled) && (eventually module(1).left_laser_enabled)", true,
		 ltl::Verdict::FALSE},
		{"omit_leg", "eventually (module(1).big_belt_speed != 0.0)", false, ltl::Verdict::UNRESOLVED},
		{"forbidden_colour", "always (module(1).led_color == 0x000000)", true, ltl::Verdict::FALSE},
	};

	for (const MutantCase& mutant : mutants) {
		::setenv("FAKE_CHILD_MUTANT", mutant.mode, 1);
		ltl::File properties = ltl::parse("prop rejects_mutant = " + std::string(mutant.formula) + ";", "mutant.spec");
		sim::Runtime runtime(7);
		runtime.load_properties(std::move(properties), 8, "mutant.spec");
		ASSERT_NO_THROW(runtime.start(8)) << mutant.mode;
		while (runtime.properties_pending() > 0) {
			runtime.advance(std::chrono::milliseconds(16), 1.0, std::chrono::seconds(5));
		}
		const auto results = runtime.property_results();
		ASSERT_EQ(results.size(), 1u);
		EXPECT_EQ(results[0].verdict, mutant.expected) << mutant.mode;
		EXPECT_NE(results[0].verdict, ltl::Verdict::TRUE) << mutant.mode << " went undetected";
		ASSERT_TRUE(results[0].decisive_tick.has_value()) << mutant.mode;
		if (mutant.should_fail_early) EXPECT_LT(*results[0].decisive_tick, 8u) << mutant.mode;
		else EXPECT_EQ(*results[0].decisive_tick, 8u) << mutant.mode;
		runtime.stop();
	}
	::unsetenv("FAKE_CHILD_MUTANT");
}

TEST_F(RuntimeTest, StrictAndBatchedModesProduceIdenticalLtlResultsAndObservationRecords) {
	struct Run {
		std::vector<sim::PropertyResult> results;
		std::string                      relevant_trace;
	};

	const auto execute = [](uint32_t batch) {
		const fs::path trace_path = fs::temp_directory_path() / ("ltlf_batch_trace_" + std::to_string(::getpid()) +
																 "_" + std::to_string(batch) + ".ndjson");
		fs::remove(trace_path);

		events::Schedule   schedule;
		events::TubArrival tub;
		tub.tick   = 6;
		tub.id     = 4;
		tub.module = module::ModuleId{FIRST};
		schedule.tubs.push_back(tub);
		ltl::File properties = ltl::parse(R"(
prop arrival = eventually appeared(tub(4));
prop absence = always !present(tub(5));
prop arrival_persists_on_the_next_logical_tick =
    always (appeared(tub(4)) -> (next present(tub(4))));
)",
										  "batch.spec");

		sim::Runtime runtime(7, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, batch);
		runtime.load(schedule);
		runtime.load_properties(std::move(properties), 12, "batch.spec");
		runtime.trace_to(trace_path.string(), {});
		runtime.start(12);
		while (runtime.properties_pending() > 0) {
			runtime.advance(std::chrono::milliseconds(48), 1.0, std::chrono::seconds(5));
		}
		Run run;
		run.results = runtime.property_results();
		runtime.stop();

		std::ifstream input(trace_path);
		std::string   line;
		while (std::getline(input, line)) {
			if (line.find("\"type\":\"property_") != std::string::npos ||
				line.find("\"type\":\"tub\"") != std::string::npos) {
				run.relevant_trace += line + "\n";
			}
		}
		fs::remove(trace_path);
		return run;
	};

	const Run strict  = execute(1);
	const Run batched = execute(3);
	ASSERT_EQ(strict.results.size(), batched.results.size());
	for (std::size_t index = 0; index < strict.results.size(); index++) {
		EXPECT_EQ(strict.results[index].name, batched.results[index].name);
		EXPECT_EQ(strict.results[index].verdict, batched.results[index].verdict);
		EXPECT_EQ(strict.results[index].decisive_tick, batched.results[index].decisive_tick);
	}
	EXPECT_EQ(strict.relevant_trace, batched.relevant_trace);
}
