// An application that crashes, hangs or ends itself stops only its own module. The simulator
// reports where it failed, in that module's context, and keeps simulating the others.
#include "../../src/firmware/crash.h"
#include "../../src/firmware/inprocess_backend.h"
#include "../../src/platform/platform.h"
#include "../../src/settings.h"
#include "../../src/sim/runtime.h"
#include "../../src/world/calibration.h"
#include "../file_cleanup.h"
#include "../os.h"
#include "firmware_images.h"
#include "scripted_image.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <unistd.h>

namespace {
	bool contains(const std::string& text, const std::string& part) {
		return text.find(part) != std::string::npos;
	}

	// The first numbered line of a report's stack trace.
	std::string first_frame(const std::string& report) {
		std::istringstream lines(report);
		for (std::string line; std::getline(lines, line);)
			if (contains(line, "   0: ")) return line;
		return {};
	}

	struct ScriptedCrash {
		explicit ScriptedCrash(const char* crash, const char* after = "3") {
			test_os::set_environment("TEST_APP_CRASH_1", crash);
			test_os::set_environment("TEST_APP_CRASH_AFTER", after);
		}

		~ScriptedCrash() {
			test_os::unset_environment("TEST_APP_CRASH_1");
			test_os::unset_environment("TEST_APP_CRASH_AFTER");
		}
	};

	struct WatchdogLimit {
		uint32_t previous = firmware::crash::watchdog_limit_ms();

		explicit WatchdogLimit(uint32_t limit) { firmware::crash::set_watchdog_limit_ms(limit); }

		~WatchdogLimit() { firmware::crash::set_watchdog_limit_ms(previous); }
	};

	// Grants module 1 until its application stops, then returns how it stopped.
	std::optional<firmware::ModuleFailure> run_until_failure(firmware::InProcessBackend& backend) {
		firmware::ExecutionGrant grant;
		for (unsigned epoch = 0; epoch < 20; ++epoch) {
			grant.reset_metadata(module::ModuleId{1}, true);
			grant.granted  = true;
			grant.delta_us = 1000;
			backend.execute_epoch(std::span(&grant, 1));
			if (grant.failure) return grant.failure;
		}
		return std::nullopt;
	}

	std::optional<firmware::ModuleFailure> scripted_failure() {
		const firmware::ImageBinding binding{module::ModuleId{1}, SCRIPTED_IMAGE};
		firmware::InProcessBackend   backend(std::span(&binding, 1), 41);
		auto                         failure = run_until_failure(backend);
		// A failed application leaves nothing for teardown to trip over.
		EXPECT_NO_THROW(backend.retire(module::ModuleId{1}));
		return failure;
	}

	// How the host took the image apart after it failed: "destroy" or "release" calls.
	std::string teardown_after(const char* crash) {
		const auto path = std::filesystem::temp_directory_path() / ("qsim-teardown-" + std::to_string(getpid()));
		const test_util::FileCleanup cleanup{path};
		test_os::set_environment("TEST_APP_TEARDOWN_RECORD", path.string().c_str());
		{
			ScriptedCrash scripted(crash);
			EXPECT_TRUE(scripted_failure());
		}
		test_os::unset_environment("TEST_APP_TEARDOWN_RECORD");
		std::ifstream     input(path);
		std::stringstream calls;
		calls << input.rdbuf();
		return calls.str();
	}
} // namespace

TEST(CrashContainment, ReportsAFaultInTheModulesContextAndKeepsTheProcess) {
	ScriptedCrash crash("segv");
	const auto    failure = scripted_failure();
	ASSERT_TRUE(failure);
	EXPECT_TRUE(contains(failure->summary, "SIGSEGV"));
	EXPECT_TRUE(contains(failure->report, "Module 1 crashed: segmentation fault"));
	EXPECT_TRUE(contains(failure->report, "a NULL pointer"));
	EXPECT_TRUE(contains(failure->report, "thread:      scripted thread"));
	EXPECT_TRUE(contains(failure->report, "Stack trace:"));
	EXPECT_TRUE(contains(first_frame(failure->report), "scripted_crash_site")) << failure->report;
	EXPECT_TRUE(contains(failure->report, "last words"));
	EXPECT_TRUE(contains(failure->report, "Module 1 is stopped. The simulation continues"));
	// The trace and stop reason must be equal between runs, so the summary names no address.
	EXPECT_FALSE(contains(failure->summary, "0x")) << failure->summary;
}

TEST(CrashContainment, ContainsAbortFromTheHostCLibrary) {
	ScriptedCrash crash("abort");
	const auto    failure = scripted_failure();
	ASSERT_TRUE(failure);
	EXPECT_TRUE(contains(failure->summary, "SIGABRT"));
	EXPECT_TRUE(contains(failure->report, "scripted_crash_site"));
}

TEST(CrashContainment, ContainsATrapInstruction) {
	ScriptedCrash crash("trap");
	const auto    failure = scripted_failure();
	ASSERT_TRUE(failure);
	EXPECT_TRUE(contains(failure->summary, "trap instruction"));
	EXPECT_TRUE(contains(first_frame(failure->report), "scripted_crash_site")) << failure->report;
}

TEST(CrashContainment, StopsAGrantThatNeverReturns) {
	ScriptedCrash crash("hang");
	WatchdogLimit limit(200);
	const auto    started = std::chrono::steady_clock::now();
	const auto    failure = scripted_failure();
	ASSERT_TRUE(failure);
	EXPECT_EQ(failure->summary, "hang: a grant did not return within 200 ms");
	EXPECT_TRUE(contains(failure->report, "Module 1 hung"));
	// The endless loop is where the trace starts.
	EXPECT_TRUE(contains(first_frame(failure->report), "scripted_crash_site")) << failure->report;
	EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10));
}

TEST(CrashContainment, ReportsWhereAnApplicationEndedItself) {
	ScriptedCrash crash("ended");
	const auto    failure = scripted_failure();
	ASSERT_TRUE(failure);
	EXPECT_EQ(failure->summary, "exit(3)");
	EXPECT_TRUE(contains(failure->report, "Module 1 exited: the application called exit(3)"));
	// The hook records the application's own stack. The simulator's frames are left out.
	EXPECT_TRUE(contains(first_frame(failure->report), "scripted_crash_site")) << failure->report;
}

// A faulted image's code never runs again, so it is not destroyed. Its stacks are released.
TEST(CrashContainment, ReleasesAFaultedImageInsteadOfDestroyingIt) {
	EXPECT_EQ(teardown_after("segv"), "release\n");
}

TEST(CrashContainment, DestroysAnApplicationThatEndedItself) {
	EXPECT_EQ(teardown_after("ended"), "destroy\n");
}

TEST(CrashContainment, AGuardedCallThatReturnsIsNotAFailure) {
	firmware::crash::Capture capture;
	int                      calls = 0;
	EXPECT_TRUE(firmware::crash::run_guarded([](void* count) { ++*static_cast<int*>(count); }, &calls, capture));
	EXPECT_EQ(calls, 1);
	EXPECT_EQ(capture.cause, firmware::crash::Cause::NONE);
}

namespace {
	class CrashedModule : public ::testing::Test {
	protected:
		void SetUp() override {
			settings::install_test_defaults();
			world::set_calibration_config(world::CalibrationConfig{});
			world::set_calibration_override(world::Calibration::nominal());
			for (uint8_t id = 1; id < MAX_MODULES; id++)
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			settings::set_module_settings(module::ModuleId{TOWER_MODULE_ID}, settings::ModuleSettings{});
			settings::set_setting("module_start_offset_mean_ms", 0.0);
			settings::set_setting("module_start_offset_sd_ms", 0.0);
			settings::set_setting("module_clock_drift_sd_ppm", 0.0);
			settings::set_setting("module_tick_skip_mean_ppm", 0.0);
			settings::set_setting("module_tick_skip_sd_ppm", 0.0);
			for (const uint8_t id : {1, 2}) {
				settings::ModuleSettings module{};
				module.active = true;
				settings::set_module_settings(module::ModuleId{id}, module);
			}
		}

		void TearDown() override {
			world::clear_calibration_seed();
			world::set_calibration_config(world::CalibrationConfig{});
			for (uint8_t id = 1; id < MAX_MODULES; id++)
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
		}

		static void run_until(sim::Runtime& runtime, uint64_t tick) {
			while (runtime.tick() < tick) {
				const uint64_t before = runtime.tick();
				runtime.advance(std::chrono::microseconds(16000), 1.0, std::chrono::milliseconds(50));
				ASSERT_GT(runtime.tick(), before);
			}
		}
	};
} // namespace

TEST_F(CrashedModule, StopsOnlyThatModuleAndTheRunContinues) {
	ScriptedCrash crash("segv", "20");
	const auto path = std::filesystem::temp_directory_path() / ("qsim-crash-" + std::to_string(getpid()) + ".ndjson");
	const test_util::FileCleanup cleanup{path};

	test_app::Runtime runtime(7);
	runtime.trace_to(path.string(), {});
	ASSERT_NO_THROW(runtime.start());
	::testing::internal::CaptureStderr();
	ASSERT_NO_THROW(run_until(runtime, 300));
	const std::string console = ::testing::internal::GetCapturedStderr();

	ASSERT_EQ(runtime.crashes().size(), 1u);
	EXPECT_EQ(runtime.crashes()[0].module, module::ModuleId{1});
	EXPECT_TRUE(contains(runtime.crashes()[0].summary, "SIGSEGV"));
	EXPECT_EQ(runtime.platform().lifecycle(module::ModuleId{1}), platform::Lifecycle::STOPPED);
	EXPECT_NE(runtime.platform().lifecycle(module::ModuleId{2}), platform::Lifecycle::STOPPED);
	// Module 2 kept executing after module 1 stopped.
	EXPECT_GE(runtime.module_tick(module::ModuleId{2}), 290u);
	EXPECT_LT(runtime.module_tick(module::ModuleId{1}), 30u);
	// Every report line names the module it belongs to.
	EXPECT_TRUE(contains(console, "[Pico 1]   [crash] Module 1 crashed"));
	runtime.stop();

	std::ifstream     input(path);
	std::stringstream trace;
	trace << input.rdbuf();
	EXPECT_TRUE(contains(trace.str(), "\"type\":\"module_stopped\""));
	EXPECT_TRUE(contains(trace.str(), "\"reason\":\"crashed: segmentation fault (SIGSEGV)\""));
	EXPECT_TRUE(contains(trace.str(), "\"type\":\"run_complete\""));
}

TEST_F(CrashedModule, AnApplicationThatExitsStopsOnlyItsModule) {
	test_os::set_environment("TEST_APP_EXIT_AFTER_1", "5");
	test_app::Runtime runtime(7);
	ASSERT_NO_THROW(runtime.start());
	::testing::internal::CaptureStderr();
	EXPECT_NO_THROW(run_until(runtime, 160));
	const std::string console = ::testing::internal::GetCapturedStderr();
	test_os::unset_environment("TEST_APP_EXIT_AFTER_1");
	ASSERT_EQ(runtime.crashes().size(), 1u);
	EXPECT_EQ(runtime.crashes()[0].summary, "scripted exit");
	EXPECT_GE(runtime.module_tick(module::ModuleId{2}), 150u);
	EXPECT_TRUE(contains(console, "Module 1 crashed: scripted exit"));
	runtime.stop();
}

// The same containment with real Zephyr images, built from test/fixtures/crash_probe.
struct ProbeCase {
	const char* mode;
	const char* summary;     // Expected in the summary
	const char* alternative; // Or this, where hosts or compilers differ
};

class CrashProbe : public CrashedModule, public ::testing::WithParamInterface<ProbeCase> {};

TEST_P(CrashProbe, ReportsTheFailureAndKeepsTheOtherModuleRunning) {
	const ProbeCase& probe = GetParam();
#ifdef _WIN32
	if (std::string(probe.mode) == "recursion")
		GTEST_SKIP() << "Windows delivers stack overflow on the exhausted stack";
#endif
	const auto bindings = test_applets::bindings("crash_probe", true);
	if (!bindings) GTEST_SKIP() << "Missing crash_probe.dll";
	WatchdogLimit limit(300);

	sim::Runtime runtime(5, *bindings, {{"CRASH_MODE", probe.mode}, {"CRASH_MODULE", "1"}});
	ASSERT_NO_THROW(runtime.start());
	::testing::internal::CaptureStderr();
	ASSERT_NO_THROW(run_until(runtime, 2500));
	const std::string console = ::testing::internal::GetCapturedStderr();

	ASSERT_EQ(runtime.crashes().size(), 1u) << console;
	EXPECT_EQ(runtime.crashes()[0].module, module::ModuleId{1});
	const std::string& summary = runtime.crashes()[0].summary;
	EXPECT_TRUE(contains(summary, probe.summary) || (probe.alternative && contains(summary, probe.alternative)))
		<< summary;

	// The report names the failing function and thread, and keeps the log text not yet sent.
	EXPECT_TRUE(contains(console, "crash_probe_fault")) << console;
	EXPECT_TRUE(contains(console, "thread:      main")) << console;
	EXPECT_TRUE(contains(console, std::string("failing with ") + probe.mode)) << console;

	// Core0's safe stop turned module 1's LED off. Module 2 still runs its application.
	EXPECT_EQ(runtime.world().module(module::ModuleId{1})->led_color().raw(), 0u);
	EXPECT_EQ(runtime.world().module(module::ModuleId{2})->led_color().raw(), 0x00ff00u);
	EXPECT_NE(runtime.platform().lifecycle(module::ModuleId{2}), platform::Lifecycle::STOPPED);
	runtime.stop();
}

INSTANTIATE_TEST_SUITE_P(
	Failures, CrashProbe,
	::testing::Values(ProbeCase{"null_read", "SIGSEGV", nullptr}, ProbeCase{"abort", "abort()", nullptr},
					  ProbeCase{"exit", "exit(3)", nullptr}, ProbeCase{"k_panic", "Zephyr fatal error 4", nullptr},
					  ProbeCase{"divide", "integer division by zero", "trap instruction"},
					  ProbeCase{"recursion", "stack overflow", nullptr}, ProbeCase{"hang", "hang", nullptr}),
	[](const ::testing::TestParamInfo<ProbeCase>& info) { return std::string(info.param.mode); });
