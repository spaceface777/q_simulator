#include "../../src/event/events.h"
#include "../../src/ltl/parser.h"
#include "../../src/settings.h"
#include "../../src/sim/runtime.h"
#include "../../src/trace/trace.h"
#include "firmware_images.h"
#include <filesystem>
#include <gtest/gtest.h>
#include <unistd.h>

TEST(Runtime, EmbeddedCoroutineMatchesLegacyFutexPthreadTrace) {
	settings::install_test_defaults();
	for (uint8_t id = 1; id < MAX_MODULES; ++id) settings::set_module_settings(module::ModuleId{id}, {});
	settings::set_module_settings(module::ModuleId{TOWER_MODULE_ID}, {});
	world::set_calibration_config({});
	world::set_calibration_override(world::Calibration::nominal());
	settings::set_setting("module_start_offset_mean_ms", 8.0);
	settings::set_setting("module_start_offset_sd_ms", 3.0);
	settings::set_setting("module_clock_drift_sd_ppm", 2500.0);
	settings::set_setting("module_tick_skip_mean_ppm", 10000.0);
	settings::set_setting("module_tick_skip_sd_ppm", 2000.0);
	for (uint8_t id : {1, 2}) {
		settings::ModuleSettings module{};
		module.active = true;
		settings::set_module_settings(module::ModuleId{id}, module);
	}
	events::Schedule schedule;
	for (uint8_t id : {1, 2}) {
		events::TubArrival tub;
		tub.id           = id;
		tub.module       = module::ModuleId{id};
		tub.tick         = 100;
		tub.removed_tick = 1500;
		schedule.tubs.push_back(tub);
	}
	const auto path = std::filesystem::temp_directory_path() / ("qsim-trace-" + std::to_string(getpid()) + ".ndjson");
	// Complete trace from review/ltl: futex/pthread lockstep, tick_batch=1,
	// module-built Picolibc, thread names enabled, and boot banner disabled.
	// Explicit environment input adds an empty object to run_start. Removing only that
	// field reproduces the legacy digest above; firmware and scheduling bytes are unchanged.
	constexpr auto expected = "8c9eb160295fc5f64f68393a4a7b6f67499ded1e4ed65de613324a112784d7e5";
	for (int restart = 0; restart < 2; ++restart) {
		sim::Runtime runtime(42, *test_applets::bindings("tub_copier"));
		runtime.load(schedule);
		runtime.load_properties(ltl::parse("prop safe = always true;"), 2000);
		runtime.trace_to(path.string(), {});
		ASSERT_NO_THROW(runtime.start());
		while (runtime.tick() < 2000)
			runtime.advance(std::chrono::microseconds(1000), 1.0, std::chrono::milliseconds(50));
		runtime.stop();
		EXPECT_GT(std::filesystem::file_size(path), 10000u);
		EXPECT_EQ(trace::digest_file(path.string()), expected);
	}
	if (!HasFailure()) std::filesystem::remove(path);
	for (uint8_t id : {1, 2}) settings::set_module_settings(module::ModuleId{id}, {});
	world::clear_calibration_seed();
}
