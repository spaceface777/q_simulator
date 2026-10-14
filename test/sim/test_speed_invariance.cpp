#include "../../src/event/events.h"
#include "../../src/sim/runtime.h"
#include "firmware_images.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace {
	struct ScenarioCase {
		const char* scenario;
		const char* layout;
		const char* applet;
	};

	const ScenarioCase cases[] = {{"laser_alignment", "one_module", "laser_alignment"},
								  {"module_movement", "one_module", "module_movement"},
								  {"module_movement_idle", "one_module", "module_movement"},
								  {"per_module_variation", "default", "module_movement"}};

	class SpeedInvarianceTest : public testing::TestWithParam<ScenarioCase> {};

	std::chrono::microseconds elapsed_for_ticks(uint64_t ticks, double speed) {
		const int64_t target  = static_cast<int64_t>(ticks) * sim::TICK_DURATION.count();
		int64_t       elapsed = static_cast<int64_t>(std::llround(target / speed));
		while (static_cast<int64_t>(elapsed * speed) < target) ++elapsed;
		while (elapsed && static_cast<int64_t>((elapsed - 1) * speed) >= target) --elapsed;
		return std::chrono::microseconds(elapsed);
	}
} // namespace

TEST_P(SpeedInvarianceTest, ProducesTheSameTraceRegardlessOfSpeed) {
	const char* slow = std::getenv("RUN_SLOW_TESTS");
	if (!slow || !*slow || std::string(slow) == "0") GTEST_SKIP() << "Set RUN_SLOW_TESTS=1 for wall-clock pacing";
	const auto&                 scenario = GetParam();
	const std::filesystem::path root(REPO_ROOT);
	std::string                 reference;
	for (double speed : {0.3, 1.0, 10.0, 1000.0}) {
		settings::parse_settings(root / "settings_example.toml",
								 root / "layouts" / (std::string(scenario.layout) + ".toml"), std::nullopt);
		const auto slots = test_applets::bindings(scenario.applet);
		if (!slots) GTEST_SKIP() << "Missing applet .dll for the active layout";
		const uint64_t seed = settings::get_setting<int>("seed");
		world::configure_calibration_from_settings();
		world::set_calibration_seed(seed);
		const auto schedule =
			events::parse_events(root / "scenarios" / (std::string(scenario.scenario) + ".toml"), seed);
		const uint64_t target =
			std::max(schedule.last_tick(), schedule.properties ? schedule.properties->duration_tick : uint64_t{0});
		const auto file =
			std::filesystem::temp_directory_path() / ("quercus_speed_" + std::to_string(getpid()) + ".ndjson");
		sim::Runtime runtime(seed, *slots);
		runtime.load(schedule);
		runtime.trace_to(file.string(), {});
		runtime.start(target);
		const auto     started    = std::chrono::steady_clock::now();
		const uint64_t first_tick = runtime.tick();
		while (runtime.tick() < target) {
			const uint64_t ticks = speed == 1000.0 ? std::min<uint64_t>(50, target - runtime.tick()) : 1;
			if (speed != 1000.0) {
				const auto due = std::chrono::duration<double>((runtime.tick() - first_tick + ticks) * 0.001 / speed);
				std::this_thread::sleep_until(started +
											  std::chrono::duration_cast<std::chrono::steady_clock::duration>(due));
			}
			const uint64_t before = runtime.tick();
			runtime.advance(elapsed_for_ticks(ticks, speed), speed, std::chrono::seconds(5));
			ASSERT_GT(runtime.tick(), before);
		}
		if (speed == 1.0 || speed == 10.0) {
			const double wall_seconds =
				std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
			const double simulated_seconds = (runtime.tick() - first_tick) * 0.001;
			EXPECT_NEAR(simulated_seconds / wall_seconds, speed, speed * 0.05)
				<< "Requested speed " << speed << ", wall time " << wall_seconds << " seconds";
		}
		EXPECT_EQ(runtime.tick(), target);
		EXPECT_FALSE(runtime.failed());
		runtime.stop();
		std::ifstream      input(file, std::ios::binary);
		std::ostringstream bytes;
		bytes << input.rdbuf();
		ASSERT_FALSE(bytes.str().empty());
		if (reference.empty()) reference = bytes.str();
		else EXPECT_EQ(bytes.str(), reference) << "speed " << speed;
		std::filesystem::remove(file);
		world::clear_calibration_seed();
	}
}

INSTANTIATE_TEST_SUITE_P(AllScenarios, SpeedInvarianceTest, testing::ValuesIn(cases),
						 [](const testing::TestParamInfo<ScenarioCase>& info) {
							 return std::string(info.param.scenario);
						 });
