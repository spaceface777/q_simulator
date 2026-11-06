// Applications built with CONFIG_QUERCUS_SIM_CPU_TIME pay simulated time for the code they run,
// so work takes time as on the RP2350, and an endless loop behaves as on a module.
#include "../../src/firmware/crash.h"
#include "../../src/platform/platform.h"
#include "../../src/settings.h"
#include "../../src/sim/runtime.h"
#include "../../src/world/calibration.h"
#include "firmware_images.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>

namespace {
	class CpuTime : public ::testing::Test {
	protected:
		void SetUp() override {
			settings::install_test_defaults();
			world::set_calibration_config(world::CalibrationConfig{});
			world::set_calibration_override(world::Calibration::nominal());
			for (uint8_t id = 1; id < MAX_MODULES; id++)
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			settings::set_module_settings(module::ModuleId{TOWER_MODULE_ID}, settings::ModuleSettings{});
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

		// Runs the probe with module 1 in `mode` and returns module 1's LED colour.
		static uint32_t led_after(const char* mode, uint64_t tick) {
			const auto bindings = test_applets::bindings("cpu_probe", true);
			if (!bindings) return 0;
			sim::Runtime runtime(5, *bindings, {{"CPU_MODE", mode}, {"CPU_MODULE", "1"}});
			EXPECT_NO_THROW(runtime.start());
			run_until(runtime, tick);
			const uint32_t color = runtime.world().module(module::ModuleId{1})->led_color().raw();
			runtime.stop();
			return color;
		}
	};
} // namespace

TEST_F(CpuTime, WorkTakesSimulatedTimeAndTheSameTimeEveryRun) {
	if (test_applets::image_path("cpu_probe", true).empty()) GTEST_SKIP() << "Missing cpu_probe.dll";
	// A million loop iterations at 150 instructions per microsecond: tens of milliseconds, depending
	// on how many instructions the host compiler makes of the loop.
	const uint32_t first = led_after("work", 3000);
	EXPECT_GE(first, 20u);
	EXPECT_LE(first, 400u);
	EXPECT_EQ(led_after("work", 3000), first) << "Instruction counts must not depend on host timing";
}

TEST_F(CpuTime, AnEndlessLoopStarvesTheHeartbeatInsteadOfStoppingTheSimulator) {
	const auto bindings = test_applets::bindings("cpu_probe", true);
	if (!bindings) GTEST_SKIP() << "Missing cpu_probe.dll";
	// The loop must end its grants by itself. A watchdog stop would be recorded as a crash.
	const uint32_t previous = firmware::crash::watchdog_limit_ms();
	firmware::crash::set_watchdog_limit_ms(300);

	sim::Runtime runtime(5, *bindings, {{"CPU_MODE", "spin"}, {"CPU_MODULE", "1"}});
	ASSERT_NO_THROW(runtime.start());
	const auto started = std::chrono::steady_clock::now();
	::testing::internal::CaptureStderr();
	run_until(runtime, 6000);
	(void)::testing::internal::GetCapturedStderr();
	firmware::crash::set_watchdog_limit_ms(previous);

	EXPECT_TRUE(runtime.crashes().empty());
	// Core0 supervision stops the module whose heartbeat thread never runs, as on a module.
	EXPECT_EQ(runtime.platform().lifecycle(module::ModuleId{1}), platform::Lifecycle::STOPPED);
	EXPECT_NE(runtime.platform().stop_reason(module::ModuleId{1}).find("heartbeat"), std::string::npos)
		<< runtime.platform().stop_reason(module::ModuleId{1});
	EXPECT_NE(runtime.platform().lifecycle(module::ModuleId{2}), platform::Lifecycle::STOPPED);
	EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(20));
	runtime.stop();
}
