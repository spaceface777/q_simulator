#include "../file_cleanup.h"
#include "../os.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#ifdef __linux__
#include <sched.h>
#endif
#include <sstream>
#include <string>

#include "../../src/event/events.h"
#include "../../src/ltl/parser.h"
#include "../../src/module/module.h"
#include "../../src/settings.h"
#include "scripted_image.h"

namespace fs = std::filesystem;

namespace {
	TEST(Runtime, PinsLockstepToOneCpu) {
#ifdef __linux__
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
#endif
	}

	// Exercises host runtime semantics through the bounded image ABI.
	class RuntimeTest : public ::testing::Test {
	protected:
		static constexpr uint8_t FIRST  = 1;
		static constexpr uint8_t SECOND = 2;

		static void clear_test_app_environment() {
			test_os::unset_environment("TEST_APP_HEARTBEAT_EVERY");
			test_os::unset_environment("TEST_APP_HEARTBEAT_EVERY_1");
			test_os::unset_environment("TEST_APP_HEARTBEAT_EVERY_2");
			test_os::unset_environment("TEST_APP_HEARTBEAT_UNTIL");
			test_os::unset_environment("TEST_APP_HEARTBEAT_UNTIL_1");
			test_os::unset_environment("TEST_APP_HEARTBEAT_UNTIL_2");
			test_os::unset_environment("TEST_APP_SUBSCRIBE");
			test_os::unset_environment("TEST_APP_EXIT_AFTER");
			test_os::unset_environment("TEST_APP_QUERY_TICK");
			test_os::unset_environment("TEST_APP_INGRESS_PROBE");
			test_os::unset_environment("TEST_APP_INGRESS_PROBE_1");
			test_os::unset_environment("TEST_APP_INGRESS_PROBE_2");
			test_os::unset_environment("TEST_APP_RATE_BURST");
			test_os::unset_environment("TEST_APP_RATE_BURST_1");
			test_os::unset_environment("TEST_APP_RATE_BURST_2");
			test_os::unset_environment("TEST_APP_STARTUP_RPC");
			test_os::unset_environment("TEST_APP_NEVER_ACTIVATE");
			test_os::unset_environment("TEST_APP_RECORD");
			test_os::unset_environment("TEST_APP_RECORD_TICKS");
			test_os::unset_environment("TEST_APP_STATUS_RECORD");
			test_os::unset_environment("TEST_APP_MUTANT");
			test_os::unset_environment("TEST_APP_REPLY_TO_MESSAGE");
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

			clear_test_app_environment();
		}

		void TearDown() override {
			world::clear_calibration_seed();
			world::set_calibration_config(world::CalibrationConfig{});
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
			clear_test_app_environment();
		}

		// Grants the runtime a fixed amount of simulated time, one call per frame like the main loop.
		static void run_for(test_app::Runtime& runtime, std::chrono::milliseconds simulated) {
			const int frames = static_cast<int>(simulated.count() / 16);
			for (int frame = 0; frame < frames; frame++) {
				runtime.advance(std::chrono::microseconds(16000), 1.0, std::chrono::milliseconds(50));
			}
		}
	};

} // namespace

TEST_F(RuntimeTest, StartsApplicationsAndWaitsForThemToActivate) {
	test_app::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	// Activation is the first heartbeat, so reaching this point means the whole loop ran.
	EXPECT_TRUE(runtime.platform().is_activated(module::ModuleId{FIRST}));
	EXPECT_TRUE(runtime.platform().is_activated(module::ModuleId{SECOND}));
	EXPECT_EQ(runtime.platform().lifecycle(module::ModuleId{FIRST}), platform::Lifecycle::RUNNING);
	EXPECT_GT(runtime.tick(), 0u);

	runtime.stop();
}

TEST_F(RuntimeTest, DeliversSynchronousRpcResponsesDuringActivation) {
	test_os::set_environment("TEST_APP_STARTUP_RPC", "1");

	test_app::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());
	EXPECT_TRUE(runtime.platform().is_activated(module::ModuleId{FIRST}));
	EXPECT_TRUE(runtime.platform().is_activated(module::ModuleId{SECOND}));
	runtime.stop();
}

TEST_F(RuntimeTest, CompletesStartupAfterSupervisionStopsApplicationsThatNeverActivate) {
	test_os::set_environment("TEST_APP_NEVER_ACTIVATE", "1");
	const auto                   path = fs::temp_directory_path() / "quercus-activation-deadline.ndjson";
	const test_util::FileCleanup cleanup_path{path};
	std::string                  reference;
	for (unsigned repeat = 0; repeat < 3; ++repeat) {
		test_app::Runtime runtime(7);
		runtime.trace_to(path.string(), {});
		ASSERT_NO_THROW(runtime.start());
		EXPECT_EQ(runtime.tick(), 10001u);
		EXPECT_FALSE(runtime.failed());
		for (const uint8_t id : {FIRST, SECOND}) {
			EXPECT_FALSE(runtime.platform().is_activated(module::ModuleId{id}));
			EXPECT_EQ(runtime.platform().lifecycle(module::ModuleId{id}), platform::Lifecycle::STOPPED);
			EXPECT_EQ(runtime.platform().stop_reason(module::ModuleId{id}),
					  "did not send a first heartbeat before its activation deadline");
		}
		runtime.stop();
		std::ifstream      input(path);
		std::ostringstream bytes;
		bytes << input.rdbuf();
		for (const uint8_t id : {FIRST, SECOND}) {
			EXPECT_NE(bytes.str().find("\"type\":\"module_stopped\",\"tick\":10000,\"module\":" + std::to_string(id) +
									   ",\"reason\":\"did not send a first heartbeat before its activation deadline\""),
					  std::string::npos);
		}
		if (repeat == 0) reference = bytes.str();
		else EXPECT_EQ(bytes.str(), reference);
	}
}

TEST_F(RuntimeTest, RejectsAShortMalformedOutputBatch) {
	test_os::set_environment("TEST_APP_INGRESS_PROBE_1", "1");

	test_app::Runtime runtime(7);
	// Byte batches require complete RPC headers to recover frame boundaries.
	EXPECT_THROW(runtime.start(), std::runtime_error);
	runtime.stop();
}

TEST_F(RuntimeTest, DropsAndAccountsForAnUnexpectedResponseFrame) {
	test_os::set_environment("TEST_APP_INGRESS_PROBE_1", "2");

	test_app::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());
	EXPECT_EQ(runtime.platform().rate_sample_count(module::ModuleId{FIRST}, platform::Bucket::GLOBAL),
			  runtime.platform().rate_sample_count(module::ModuleId{SECOND}, platform::Bucket::GLOBAL) + 1);
	runtime.stop();
}

TEST_F(RuntimeTest, HeartbeatDeathStopsOnlyTheFailedModule) {
	test_os::set_environment("TEST_APP_HEARTBEAT_UNTIL_1", "100");

	test_app::Runtime runtime(7);
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
	const test_util::FileCleanup cleanup_statuses{statuses};
	test_os::set_environment("TEST_APP_STATUS_RECORD", statuses.string().c_str());

	test_app::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());
	// Strict grants honor pauses before another burst. Preload ingress to isolate the stop handshake.
	const platform::ModuleTime now{runtime.tick(), runtime.module_tick(module::ModuleId{FIRST})};
	platform::IngressAdmission admission;
	for (unsigned i = 0; i < 200 && !admission.stop_triggered; ++i)
		admission = runtime.platform().account_ingress(module::ModuleId{FIRST}, now);
	ASSERT_TRUE(admission.stop_triggered);
	rpc::Frame request;
	request.type    = MT_REQ;
	request.cmd     = CMD_LASER_LEFT_GET;
	request.call_id = 0x1234;
	auto terminal   = runtime.platform().handle_request(module::ModuleId{FIRST}, request, now, admission);
	ASSERT_TRUE(terminal.has_value());
	ASSERT_TRUE(terminal->stop_after_delivery);
	runtime.platform().enqueue(module::ModuleId{FIRST}, std::move(*terminal));
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
	EXPECT_EQ(runtime.tick() - world_before, 100u);
	EXPECT_EQ(runtime.module_tick(module::ModuleId{SECOND}) - live_before, 100u);

	runtime.stop();
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

	test_app::Runtime runtime(7);
	runtime.load(schedule);
	ASSERT_NO_THROW(runtime.start());
	EXPECT_GT(runtime.tick(), tub.tick);
	EXPECT_NE(runtime.world().tub(tub.id), nullptr);
	runtime.stop();
}

TEST_F(RuntimeTest, TracesScheduledAndYeetedTubRemovalsOnceWithTheirReason) {
	const fs::path trace_path =
		fs::temp_directory_path() / ("quercus_tub_removal_trace_" + std::to_string(::getpid()) + ".ndjson");
	const test_util::FileCleanup cleanup_trace_path{trace_path};

	events::Schedule   schedule;
	events::TubArrival scheduled;
	scheduled.tick         = 0;
	scheduled.removed_tick = 2;
	scheduled.id           = 4;
	scheduled.module       = module::ModuleId{FIRST};
	schedule.tubs.push_back(scheduled);

	test_app::Runtime runtime(7);
	runtime.load(schedule);
	runtime.world().place(5, module::ModuleId{FIRST}, world::Position::CENTER, RfidPayload{});
	runtime.world().module(module::ModuleId{FIRST})->set_servo_angle(module::ServoAngle{0});
	runtime.world().module(module::ModuleId{FIRST})->set_big_belt_speed(module::BeltSpeed{100});
	runtime.trace_to(trace_path.string(), {});
	ASSERT_NO_THROW(runtime.start());
	const uint64_t endpoint = runtime.tick() + 3000;
	while (runtime.tick() < endpoint) {
		const uint64_t before = runtime.tick();
		ASSERT_NO_THROW(runtime.advance(std::chrono::milliseconds(1), 1.0, std::chrono::seconds(5)));
		ASSERT_GT(runtime.tick(), before);
	}
	EXPECT_EQ(runtime.world().tub(4), nullptr);
	EXPECT_EQ(runtime.world().tub(5), nullptr);
	runtime.stop();

	const world::Calibration calibration  = world::Calibration::nominal();
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
	test_app::Runtime runtime(7);
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
	const test_util::FileCleanup cleanup_trace_path{trace_path};
	test_app::Runtime            runtime(7);
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
	const test_util::FileCleanup cleanup_trace_path{trace_path};
	test_app::Runtime            runtime(7);
	runtime.trace_to(trace_path.string(), {});
	ASSERT_NO_THROW(runtime.start(4));
	runtime.stop();

	std::ifstream      input(trace_path);
	std::ostringstream trace;
	trace << input.rdbuf();
	EXPECT_NE(trace.str().find(
				  "\"type\":\"module_clock\",\"module\":1,\"start_tick\":4,\"rate_ppm\":1000000,\"skip_ppm\":1000000"),
			  std::string::npos);
	EXPECT_NE(trace.str().find(
				  "\"type\":\"module_clock\",\"module\":2,\"start_tick\":4,\"rate_ppm\":1000000,\"skip_ppm\":1000000"),
			  std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"module_tick_skipped\",\"tick\":4,\"module\":1"), std::string::npos);
}

TEST_F(RuntimeTest, AdvancesSimulatedTimeInProportionToElapsedTime) {
	test_app::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	const uint64_t started = runtime.tick();
	run_for(runtime, std::chrono::milliseconds(160));

	// The host deadline can prevent completion of all 160 ticks.
	const uint64_t advanced = runtime.tick() - started;
	EXPECT_GT(advanced, 0u);
	EXPECT_LE(advanced, 160u);

	runtime.stop();
}

TEST_F(RuntimeTest, UsesTheTimeDeadlineRatherThanAnArbitraryTickLimit) {
	test_app::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	const uint64_t started = runtime.tick();
	runtime.advance(std::chrono::milliseconds(300), 1.0, std::chrono::seconds(5));

	EXPECT_EQ(runtime.tick() - started, 300u);

	runtime.stop();
}

TEST_F(RuntimeTest, ReplaysEveryTickOfAnAdvance) {
	test_app::Runtime runtime(7);
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

TEST_F(RuntimeTest, TickSkippingProducesTheSameTraceAcrossAdvanceSizes) {
	settings::set_setting("module_tick_skip_mean_ppm", 300'000.0);
	settings::set_setting("module_tick_skip_sd_ppm", 0.0);
	settings::set_setting("module_clock_drift_sd_ppm", 137.0);
	test_os::set_environment("TEST_APP_HEARTBEAT_EVERY", "4294967295");

	const auto execute = [](uint32_t batch) {
		const fs::path path = fs::temp_directory_path() / ("quercus_skip_trace_" + std::to_string(::getpid()) + "_" +
														   std::to_string(batch) + ".ndjson");
		const test_util::FileCleanup cleanup_path{path};
		test_app::Runtime            runtime(7);
		runtime.trace_to(path.string(), {});
		runtime.start();
		const uint64_t before = runtime.tick();
		for (unsigned advanced = 0; advanced < 300; advanced += batch)
			runtime.advance(std::chrono::milliseconds(std::min<unsigned>(batch, 300 - advanced)), 1.0,
							std::chrono::seconds(10));
		EXPECT_EQ(runtime.tick() - before, 300u);
		runtime.stop();

		std::ifstream      input(path);
		std::ostringstream trace;
		trace << input.rdbuf();
		return trace.str();
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
	test_os::set_environment("TEST_APP_QUERY_TICK", "2");

	const fs::path trace_path =
		fs::temp_directory_path() / ("quercus_local_rpc_delay_" + std::to_string(::getpid()) + ".ndjson");
	const test_util::FileCleanup cleanup_trace_path{trace_path};
	test_app::Runtime            runtime(7);
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
	ASSERT_TRUE(request_tick.has_value());
	ASSERT_TRUE(response_tick.has_value());
	EXPECT_GT(*response_tick, *request_tick + sim::RPC_RX_DELAY_LOCAL_TICKS + sim::RPC_TX_DELAY_LOCAL_TICKS)
		<< "90% skipped module ticks must stretch local RPC service delays in global time";
}

TEST_F(RuntimeTest, RetainsPendingResponsesAcrossAdvanceCalls) {
	const fs::path record = fs::temp_directory_path() / ("quercus_child_record_" + std::to_string(::getpid()));
	const test_util::FileCleanup cleanup_record{record};
	test_os::set_environment("TEST_APP_QUERY_TICK", "2");
	test_os::set_environment("TEST_APP_RECORD", record.string().c_str());

	test_app::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	// A response queued during one advance must survive until the next grant consumes it.
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
}

TEST_F(RuntimeTest, WarnsOnlyAfterTenFramesOfSustainedPacingShortfall) {
	test_app::Runtime runtime(7);
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
	test_app::Runtime runtime(7);
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
	const test_util::FileCleanup cleanup_record{record};
	test_os::set_environment("TEST_APP_SUBSCRIBE", std::to_string(EVENT_RFID_DETECT).c_str());
	test_os::set_environment("TEST_APP_RECORD", record.string().c_str());

	events::Schedule schedule;

	events::TubArrival tub;
	tub.tick   = 10;
	tub.id     = 4;
	tub.module = module::ModuleId{FIRST};
	schedule.tubs.push_back(tub);

	test_app::Runtime runtime(7);
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
}

TEST_F(RuntimeTest, HardwareEventsIncludeConfiguredDelayAndNextTickHandoff) {
	const fs::path record = fs::temp_directory_path() / ("quercus_hardware_event_ticks_" + std::to_string(::getpid()));
	const test_util::FileCleanup cleanup_record{record};
	test_os::set_environment("TEST_APP_SUBSCRIBE", std::to_string(EVENT_LASER_LEFT_DETECT | EVENT_RFID_DETECT).c_str());
	test_os::set_environment("TEST_APP_RECORD", record.string().c_str());
	test_os::set_environment("TEST_APP_RECORD_TICKS", "1");
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

	test_app::Runtime runtime(7);
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
}

TEST_F(RuntimeTest, FailsTheRunWhenAnApplicationExits) {
	test_os::set_environment("TEST_APP_EXIT_AFTER", "5");
	const auto                   path = fs::temp_directory_path() / "quercus-failed-grant.ndjson";
	const test_util::FileCleanup cleanup_path{path};

	test_app::Runtime runtime(7);
	runtime.trace_to(path.string(), {});
	ASSERT_NO_THROW(runtime.start());

	// Carrying on would produce a run the inputs no longer describe, so the run ends instead.
	EXPECT_THROW(run_for(runtime, std::chrono::milliseconds(160)), std::runtime_error);
	std::ifstream      input(path);
	std::ostringstream bytes;
	bytes << input.rdbuf();
	EXPECT_NE(bytes.str().find("\"type\":\"rpc_request\""), std::string::npos);
	EXPECT_EQ(bytes.str().find("\"type\":\"run_complete\""), std::string::npos);

	runtime.stop();
}

TEST_F(RuntimeTest, WaitsForEveryApplicationBeforeCompletingATick) {
	test_app::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());

	// Every module must complete its grant before the world completes a tick.
	for (int frame = 0; frame < 5; frame++) {
		const uint64_t before = runtime.tick();
		runtime.advance(std::chrono::microseconds(16000), 1.0, std::chrono::milliseconds(200));
		EXPECT_GT(runtime.tick(), before);
	}

	runtime.stop();
}

TEST_F(RuntimeTest, StartupReportsMissingFirmwareKeyOrImagePath) {
	const auto folder =
		fs::temp_directory_path() /
		("quercus-missing-firmware-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directory(folder);
	const auto settings_file = folder / "settings.toml";
	const auto layout_file   = folder / "layout.toml";
	const auto missing_image = folder / "missing.dll";
	std::ofstream(layout_file) << "[module.1]\nactive=true\nneighbors=[0,0,0]\npos=[0,0]\nangle=0\n";
	for (const bool has_path : {false, true}) {
		std::ofstream settings_file_output(settings_file);
		settings_file_output << "simulation_speed=1.0\nscale=1.0\nseed=7\n";
		if (has_path) settings_file_output << "firmware=\"" << missing_image.string() << "\"\n";
		settings_file_output.close();
		ASSERT_NO_THROW(settings::parse_settings(settings_file, layout_file));
		sim::Runtime runtime(7);
		try {
			runtime.start();
			FAIL() << "Started without a firmware image";
		} catch (const std::exception& error) {
			EXPECT_NE(std::string(error.what()).find("Module 1"), std::string::npos);
			EXPECT_NE(std::string(error.what()).find(has_path ? missing_image.string() : "firmware"),
					  std::string::npos);
		}
	}
	fs::remove_all(folder);
}

TEST_F(RuntimeTest, TreatsAnEmptyPropertyFileAsAVacuousPass) {
	ltl::File properties = ltl::parse("", "empty.spec");

	test_app::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 0, "empty.spec");
	ASSERT_NO_THROW(runtime.start());
	EXPECT_EQ(runtime.properties_total(), 0u);
	EXPECT_EQ(runtime.properties_pending(), 0u);
	EXPECT_FALSE(runtime.failed());
	runtime.stop();
}

TEST_F(RuntimeTest, ReportsTheImportedFileAsAPropertySource) {
	const fs::path directory = fs::temp_directory_path() / "q_simulator_runtime_import_location_test";
	fs::remove_all(directory);
	fs::create_directories(directory);
	const fs::path imported_file = directory / "imported.spec";
	{
		std::ofstream imported(imported_file);
		imported << "prop imported_property = true;\n";
	}
	const fs::path main_file  = directory / "main.spec";
	ltl::File      properties = ltl::parse("import \"imported.spec\";\n", main_file.string());

	test_app::Runtime runtime(7);
	runtime.load_properties(std::move(properties), 1, main_file.string());
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0].source, fs::absolute(imported_file).lexically_normal().string());
	EXPECT_EQ(results[0].position.line, 1u);
	fs::remove_all(directory);
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

	test_app::Runtime runtime(7);
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

	test_app::Runtime runtime(7);
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

	test_app::Runtime runtime(7);
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

	test_app::Runtime runtime(7);
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

	test_app::Runtime runtime(7);
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

	test_app::Runtime runtime(7);
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

TEST_F(RuntimeTest, WakesAllSubscriberBitsetWordsOnATubPulse) {
	events::Schedule   schedule;
	events::TubArrival tub;
	tub.tick         = 25;
	tub.removed_tick = 30;
	tub.id           = 4;
	tub.module       = module::ModuleId{FIRST};
	schedule.tubs.push_back(tub);

	std::ostringstream source;
	for (unsigned index = 0; index < 130; index++)
		source << "prop pulse_" << index << " = F[0ms, 50ms] appeared(tub(4));\n";
	source << "prop future during [22ms, 50ms] = F appeared(tub(4));\n"
		   << "prop new_domain = F (exists item: Tub: appeared(item));\n"
		   << "prop clearing = G (appeared(tub(4)) -> (X (!appeared(tub(4)))));\n"
		   << "prop removal = F disappeared(tub(4));\n";
	test_app::Runtime runtime(7);
	runtime.load(schedule);
	runtime.load_properties(ltl::parse(source.str(), "subscriptions.spec"), 50, "subscriptions.spec");
	ASSERT_NO_THROW(runtime.start(50));
	while (runtime.properties_pending() && runtime.tick() <= 50)
		runtime.advance(std::chrono::milliseconds(16), 1.0, std::chrono::seconds(5));
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 134u);
	for (std::size_t index = 0; index < 132; index++) {
		EXPECT_EQ(results[index].verdict, ltl::Verdict::TRUE) << results[index].name;
		EXPECT_EQ(results[index].decisive_tick, 25u) << results[index].name;
	}
	EXPECT_EQ(results[132].verdict, ltl::Verdict::TRUE);
	EXPECT_EQ(results[132].decisive_tick, 50u);
	EXPECT_EQ(results[132].activations, 1u);
	EXPECT_EQ(results[133].verdict, ltl::Verdict::TRUE);
	EXPECT_EQ(results[133].decisive_tick, 30u);
	runtime.stop();
}

TEST_F(RuntimeTest, UpdatesSubscriptionsWhenAShortCircuitChangesItsReadSet) {
	test_app::Runtime runtime(7);
	runtime.load_properties(ltl::parse("prop guarded = G (module(1).big_belt_speed == 0 || module(2).led_value == 0);",
									   "subscriptions.spec"),
							100, "subscriptions.spec");
	ASSERT_NO_THROW(runtime.start(0));
	auto* first  = runtime.world().module(module::ModuleId{FIRST});
	auto* second = runtime.world().module(module::ModuleId{SECOND});
	ASSERT_NE(first, nullptr);
	ASSERT_NE(second, nullptr);

	first->set_big_belt_speed(module::BeltSpeed{25});
	runtime.advance(std::chrono::milliseconds(1), 1.0, std::chrono::seconds(5));
	EXPECT_FALSE(runtime.failed()); // The second operand becomes a dependency.
	first->set_big_belt_speed(module::BeltSpeed{0});
	runtime.advance(std::chrono::milliseconds(1), 1.0, std::chrono::seconds(5));
	second->set_led_color(module::LedColor{1});
	runtime.advance(std::chrono::milliseconds(1), 1.0, std::chrono::seconds(5));
	EXPECT_FALSE(runtime.failed()); // Its old dependency is gone while the guard holds.
	const uint64_t failure_tick = runtime.tick();
	first->set_big_belt_speed(module::BeltSpeed{25});
	runtime.advance(std::chrono::milliseconds(1), 1.0, std::chrono::seconds(5));
	ASSERT_TRUE(runtime.failed());
	EXPECT_EQ(runtime.property_results()[0].decisive_tick, failure_tick);
	runtime.stop();
}

TEST_F(RuntimeTest, ReportsSleepingTriggerCountsExactlyAfterAnEarlyFailure) {
	test_app::Runtime runtime(7);
	runtime.load_properties(ltl::parse(R"(
prop counted = G (true -> module(1).led_value == 0);
prop halt at 20ms = false;
)",
									   "subscriptions.spec"),
							100, "subscriptions.spec");
	ASSERT_NO_THROW(runtime.start(20));
	while (!runtime.failed()) runtime.advance(std::chrono::milliseconds(16), 1.0, std::chrono::seconds(5));
	EXPECT_EQ(runtime.tick(), 21u);
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 2u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::PENDING);
	EXPECT_EQ(results[0].activations, 21u);
	EXPECT_EQ(results[1].decisive_tick, 20u);
	EXPECT_EQ(runtime.property_results()[0].activations, 21u) << "Reporting does not consume repeats";
	runtime.stop();
}

TEST_F(RuntimeTest, SleepingTriggerCountsDoNotExtendPastTheSemanticScenarioEnd) {
	test_app::Runtime runtime(7);
	runtime.load_properties(
		ltl::parse("prop counted during [1ms, 100ms] = G (true -> module(1).led_value == 0);", "subscriptions.spec"),
		10, "subscriptions.spec");
	ASSERT_NO_THROW(runtime.start(20));
	while (runtime.tick() <= 20) runtime.advance(std::chrono::milliseconds(1), 1.0, std::chrono::seconds(5));
	const auto results = runtime.property_results();
	ASSERT_EQ(results.size(), 1u);
	EXPECT_EQ(results[0].verdict, ltl::Verdict::PENDING);
	EXPECT_EQ(results[0].activations, 10u);
	runtime.stop();
}

TEST_F(RuntimeTest, StopsWhenAPropertyFails) {
	ltl::File properties =
		ltl::parse("prop impossible at 1ms = module(1).rfid_active && !module(1).rfid_active;", "runtime.spec");

	test_app::Runtime runtime(7);
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
	test_app::Runtime runtime(7);
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
	const test_util::FileCleanup cleanup_trace_path{trace_path};

	events::Schedule   schedule;
	events::TubArrival tub;
	tub.tick   = 0;
	tub.id     = 4;
	tub.module = module::ModuleId{FIRST};
	schedule.tubs.push_back(tub);

	ltl::File         properties = ltl::parse("prop broken at 0ms = !present(tub(4));", "failure.spec");
	test_app::Runtime runtime(7);
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
	EXPECT_NE(trace.str().find("\"type\":\"property_activation\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"property_verdict\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"verdict\":\"false\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"property_fact\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"type\":\"tub\",\"tick\":0,\"id\":4"), std::string::npos);
}

TEST_F(RuntimeTest, RecordsMetricObligationCreationAndDeadline) {
	const fs::path trace_path =
		fs::temp_directory_path() / ("ltlf_deadline_trace_" + std::to_string(::getpid()) + ".ndjson");
	const test_util::FileCleanup cleanup_trace_path{trace_path};

	ltl::File properties =
		ltl::parse("prop deadline = eventually[1ms, 2ms] module(1).led_color == 0xffffff;", "deadline.spec");
	test_app::Runtime runtime(7);
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
	EXPECT_NE(trace.str().find("\"ltlf_version\":\"0.3\""), std::string::npos);
	EXPECT_NE(trace.str().find("\"obligation_created_tick\":0"), std::string::npos);
	EXPECT_NE(trace.str().find("\"deadline_tick\":2"), std::string::npos);
	EXPECT_NE(trace.str().find("\"actual\":\"0x000000\""), std::string::npos);
}

TEST_F(RuntimeTest, ObservesRpcTrafficAtExactLogicalBoundaries) {
	const fs::path trace_path =
		fs::temp_directory_path() / ("ltlf_rpc_trace_" + std::to_string(::getpid()) + ".ndjson");
	const test_util::FileCleanup cleanup_trace_path{trace_path};
	test_os::set_environment("TEST_APP_QUERY_TICK", "2");

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

	test_app::Runtime runtime(7);
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
	const std::string records = trace.str();

	// The receive and transmit delays each consume one executed module tick.
	const std::string request =
		"\"type\":\"rpc_request\",\"tick\":2,\"id\":3,\"module\":1,\"cmd\":" + std::to_string(CMD_LASER_LEFT_GET);
	const std::string response =
		"\"type\":\"rpc_response\",\"tick\":4,\"id\":3,\"request_id\":3,\"module\":1,\"cmd\":" +
		std::to_string(CMD_LASER_LEFT_GET);
	EXPECT_NE(records.find(request), std::string::npos);
	EXPECT_NE(records.find(response), std::string::npos) << records;
}

TEST_F(RuntimeTest, MessageDeliveryProducesThePayloadCheckedReplyOnExactlyTheNextTick) {
	test_os::set_environment("TEST_APP_REPLY_TO_MESSAGE", "1");
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

	test_app::Runtime runtime(7);
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
		test_os::set_environment("TEST_APP_MUTANT", mutant.mode);
		ltl::File properties = ltl::parse("prop rejects_mutant = " + std::string(mutant.formula) + ";", "mutant.spec");
		test_app::Runtime runtime(7);
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
	test_os::unset_environment("TEST_APP_MUTANT");
}

TEST_F(RuntimeTest, AdvanceSizesProduceIdenticalLtlResultsAndObservationRecords) {
	struct Run {
		std::vector<sim::PropertyResult> results;
		std::string                      relevant_trace;
	};

	const auto execute = [](uint32_t batch) {
		const fs::path trace_path = fs::temp_directory_path() / ("ltlf_batch_trace_" + std::to_string(::getpid()) +
																 "_" + std::to_string(batch) + ".ndjson");
		const test_util::FileCleanup cleanup_trace_path{trace_path};

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

		test_app::Runtime runtime(7);
		runtime.load(schedule);
		runtime.load_properties(std::move(properties), 12, "batch.spec");
		runtime.trace_to(trace_path.string(), {});
		runtime.start(12);
		while (runtime.properties_pending() > 0) {
			runtime.advance(std::chrono::milliseconds(batch), 1.0, std::chrono::seconds(5));
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

TEST_F(RuntimeTest, LtlFailureInsideAnAdvanceStopsAtTheExactTickAndKeepsTheTraceIdentical) {
	test_os::set_environment("TEST_APP_HEARTBEAT_UNTIL", "0");

	const auto execute = [](uint32_t advance_ticks, const std::string& suffix) {
		const fs::path path =
			fs::temp_directory_path() / ("quercus_exact_ltl_" + std::to_string(::getpid()) + suffix + ".ndjson");
		const test_util::FileCleanup cleanup_path{path};
		ltl::File         properties = ltl::parse("prop fails during [10ms, 20ms] = always false;", "exact.spec");
		test_app::Runtime runtime(7);
		runtime.load_properties(std::move(properties), 20, "exact.spec");
		runtime.trace_to(path.string(), {});
		runtime.start();
		for (unsigned advanced = 0; advanced < 50 && !runtime.halted(); advanced += advance_ticks)
			runtime.advance(std::chrono::milliseconds(std::min<unsigned>(advance_ticks, 50 - advanced)), 1.0,
							std::chrono::seconds(5));
		const auto results = runtime.property_results();
		EXPECT_TRUE(runtime.failed());
		EXPECT_EQ(runtime.tick(), 11u);
		EXPECT_EQ(results.size(), 1u);
		if (!results.empty()) {
			EXPECT_EQ(results.front().decisive_tick, 10u);
		}
		runtime.stop();
		std::ifstream      input(path, std::ios::binary);
		std::ostringstream contents;
		contents << input.rdbuf();
		return contents.str();
	};

	const std::string strict = execute(1, "_single");
	const std::string exact  = execute(50, "_wide");
	EXPECT_EQ(exact, strict);
}
