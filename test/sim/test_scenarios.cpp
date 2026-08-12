#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <toml++/toml.hpp>
#include <vector>

#include "../../src/event/events.h"
#include "../../src/ltl/parser.h"
#include "../../src/settings.h"
#include "../../src/sim/runtime.h"
#include "../../src/world/calibration.h"
#include "../../src/world/world.h"

namespace fs = std::filesystem;

namespace {

	// Skips unless a real applet binary is built; see external/quercus_applets, built via
	// `(cd external/quercus_applets && ./quercus_sdk/qbuild sim)`.
	std::string applet_path(const std::string& applet) {
		const fs::path path = fs::path(REPO_ROOT) / "external" / "quercus_applets" / "out" / (applet + ".exe");
		if (!fs::exists(path)) {
			return "";
		}
		return path.string();
	}

	struct ScenarioCase {
		std::string scenario; // file under scenarios/
		std::string layout;   // file under layouts/, matching the -l the scenario's header documents
		std::string applet;   // applet name under external/quercus_applets/out
		uint32_t    tick_batch;
	};

	// Mirrors the "Run it with" comment at the top of each file in scenarios/.
	const std::vector<ScenarioCase> kScenarios = [] {
		const ScenarioCase base[] = {
			{"hardware_test.toml", "one_module.toml", "hardware_test", 1},
			{"hardware_test_blocked_beam.toml", "one_module.toml", "hardware_test", 1},
			{"hardware_test_tag_at_startup.toml", "one_module.toml", "hardware_test", 1},
			{"laser_alignment.toml", "one_module.toml", "laser_alignment", 1},
			{"module_movement.toml", "one_module.toml", "module_movement", 1},
			{"module_movement_idle.toml", "one_module.toml", "module_movement", 1},
			{"per_module_variation.toml", "default.toml", "module_movement", 1},
		};
		std::vector<ScenarioCase> result;
		for (const ScenarioCase& scenario : base) {
			for (const uint32_t tick_batch : {1u, 5u, 15u}) {
				result.push_back({scenario.scenario, scenario.layout, scenario.applet, tick_batch});
			}
		}
		return result;
	}();

	// parse_settings validates application_path against disk, so it must be baked in before parsing.
	std::string settings_with_binary(const fs::path& example, const std::string& binary) {
		toml::table table = toml::parse_file(example.string());
		table.insert_or_assign("application_path", binary);

		const fs::path temp =
			fs::temp_directory_path() / ("scenario_test_settings_" + std::to_string(::getpid()) + ".toml");
		std::ofstream out(temp);
		out << table;
		return temp.string();
	}

	std::string scenario_name(const ::testing::TestParamInfo<ScenarioCase>& info) {
		const std::string& file = info.param.scenario;
		return file.substr(0, file.rfind(".toml")) + "_batch_" + std::to_string(info.param.tick_batch);
	}

	ltl::File parse_properties(const fs::path& path) {
		std::ifstream input(path);
		if (!input) throw std::runtime_error("Could not open " + path.string());
		std::ostringstream source;
		source << input.rdbuf();
		return ltl::parse(source.str(), path.string());
	}

	class ScenarioTest : public ::testing::TestWithParam<ScenarioCase> {};

} // namespace

// Runs every scenario in scenarios/ against the applet it documents, as simulation-scenario does.
TEST_P(ScenarioTest, AllLtlPropertiesHold) {
	const ScenarioCase& scenario = GetParam();
	const std::string   binary   = applet_path(scenario.applet);
	if (binary.empty()) {
		GTEST_SKIP() << "No " << scenario.applet
					 << " applet built; run (cd external/quercus_applets && ./quercus_sdk/qbuild sim)";
	}

	const fs::path    root(REPO_ROOT);
	const std::string settings_path = settings_with_binary(root / "settings_example.toml", binary);
	settings::parse_settings(settings_path, root / "layouts" / scenario.layout);

	const uint64_t         seed     = static_cast<uint64_t>(settings::get_setting<int>("seed"));
	const events::Schedule schedule = events::parse_events(root / "scenarios" / scenario.scenario, seed);
	ASSERT_TRUE(schedule.properties.has_value());
	ltl::File properties = parse_properties(schedule.properties->property_file);

	world::configure_calibration_from_settings();
	world::set_calibration_seed(seed);

	sim::Runtime runtime(seed, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, scenario.tick_batch);
	runtime.load(schedule);
	runtime.load_properties(std::move(properties), schedule.properties->duration_tick,
							schedule.properties->property_file.string());
	ASSERT_NO_THROW(runtime.start(schedule.properties->duration_tick));

	// Matches the scenario runner's pacing and inclusive finite-trace endpoint.
	constexpr std::chrono::microseconds SLICE{60000};
	constexpr std::chrono::milliseconds CHILD_BUDGET{5000};

	bool stalled = false;
	while (!runtime.failed() && runtime.properties_pending() > 0) {
		const uint64_t before = runtime.tick();
		ASSERT_LE(before, schedule.properties->duration_tick);
		const uint64_t ticks_left  = schedule.properties->duration_tick - before + 1;
		const uint64_t slice_ticks = static_cast<uint64_t>(SLICE.count() / sim::TICK_DURATION.count());
		uint64_t       granted     = std::min(ticks_left, slice_ticks);
		if (granted % scenario.tick_batch != 0) {
			granted += scenario.tick_batch - (granted % scenario.tick_batch);
		}
		runtime.advance(std::chrono::microseconds(granted * sim::TICK_DURATION.count()), 1.0, CHILD_BUDGET);
		if (runtime.tick() == before) {
			stalled = true;
			break;
		}
	}

	const bool  failed = runtime.failed();
	std::string detail;

	for (const sim::PropertyResult& result : runtime.property_results()) {
		if (result.verdict == ltl::Verdict::TRUE) continue;
		detail += result.name + " was " +
				  (result.verdict == ltl::Verdict::FALSE ? std::string("false") : std::string("still pending"));
		if (result.decisive_tick) detail += " at tick " + std::to_string(*result.decisive_tick);
		detail += "\n";
	}

	runtime.stop();

	EXPECT_FALSE(stalled) << "applications stopped making progress at tick " << runtime.tick();
	EXPECT_FALSE(failed) << detail;
	EXPECT_EQ(runtime.properties_pending(), 0u) << detail;
}

INSTANTIATE_TEST_SUITE_P(AllScenarios, ScenarioTest, ::testing::ValuesIn(kScenarios), scenario_name);

TEST(ScenarioSeedCorpus, ModuleMovementPassesTheStableV01SeedsWithoutSpecEdits) {
	const std::string binary = applet_path("module_movement");
	if (binary.empty()) {
		GTEST_SKIP()
			<< "No module_movement applet built; run (cd external/quercus_applets && ./quercus_sdk/qbuild sim)";
	}

	// The short corpus runs on every commit. The documented large corpus is selected by scheduled CI
	// with LTL_FULL_SEED_CORPUS=1; keeping both lists here prevents the two jobs from silently drifting.
	const std::vector<uint64_t>  short_seeds = {7, 42, 31337};
	const std::vector<uint64_t>  full_seeds  = {1, 2, 3, 4, 5, 7, 11, 17, 23, 42, 67, 101, 257, 1024, 31337, 20260809};
	const std::vector<uint64_t>& seeds =
		(::getenv("LTL_FULL_SEED_CORPUS") == nullptr && ::getenv("RUN_SLOW_TESTS") == nullptr) ? short_seeds
																							   : full_seeds;
	const fs::path root(REPO_ROOT);
	const fs::path scenario_path = root / "scenarios" / "module_movement.toml";

	for (const uint64_t seed : seeds) {
		const std::string settings_path = settings_with_binary(root / "settings_example.toml", binary);
		settings::parse_settings(settings_path, root / "layouts" / "one_module.toml");
		settings::set_setting("seed", static_cast<int>(seed));
		const events::Schedule schedule = events::parse_events(scenario_path, seed);
		ASSERT_TRUE(schedule.properties.has_value());
		ltl::File properties = parse_properties(schedule.properties->property_file);

		world::configure_calibration_from_settings();
		world::set_calibration_seed(seed);
		sim::Runtime runtime(seed);
		runtime.load(schedule);
		runtime.load_properties(std::move(properties), schedule.properties->duration_tick,
								schedule.properties->property_file.string());
		ASSERT_NO_THROW(runtime.start(schedule.properties->duration_tick)) << "seed " << seed;

		constexpr std::chrono::microseconds SLICE{50000};
		constexpr std::chrono::milliseconds CHILD_BUDGET{5000};
		while (!runtime.failed() && runtime.properties_pending() > 0) {
			const uint64_t before      = runtime.tick();
			const uint64_t ticks_left  = schedule.properties->duration_tick - before + 1;
			const uint64_t slice_ticks = static_cast<uint64_t>(SLICE.count() / sim::TICK_DURATION.count());
			runtime.advance(std::chrono::microseconds(std::min(ticks_left, slice_ticks) * sim::TICK_DURATION.count()),
							1.0, CHILD_BUDGET);
			ASSERT_GT(runtime.tick(), before) << "seed " << seed;
		}

		std::string detail;
		for (const sim::PropertyResult& result : runtime.property_results()) {
			if (result.verdict != ltl::Verdict::TRUE) detail += result.name + " did not hold\n";
		}
		EXPECT_FALSE(runtime.failed()) << "seed " << seed << "\n" << detail;
		EXPECT_EQ(runtime.properties_pending(), 0u) << "seed " << seed << "\n" << detail;
		runtime.stop();
		fs::remove(settings_path);
	}
}

TEST(ScenarioClockLoss, ExistingModuleMovementScenarioIsSafeWithThirtyPercentSkippedTicks) {
	const std::string binary = applet_path("module_movement");
	if (binary.empty()) {
		GTEST_SKIP() << "No module_movement applet built; run qbuild sim module_movement";
	}

	const fs::path    root(REPO_ROOT);
	const std::string settings_path = settings_with_binary(root / "settings_example.toml", binary);
	settings::parse_settings(settings_path, root / "layouts" / "one_module.toml");
	settings::set_setting("seed", 42);
	settings::set_setting("module_start_offset_mean_ms", 300.0);
	settings::set_setting("module_start_offset_sd_ms", 100.0);
	settings::set_setting("module_clock_drift_sd_ppm", 100.0);
	settings::set_setting("module_tick_skip_mean_ppm", 300'000.0);
	settings::set_setting("module_tick_skip_sd_ppm", 0.0);

	const events::Schedule schedule = events::parse_events(root / "scenarios" / "module_movement.toml", 42);
	ASSERT_TRUE(schedule.properties.has_value());
	ltl::File properties = parse_properties(schedule.properties->property_file);
	world::configure_calibration_from_settings();
	world::set_calibration_seed(42);

	sim::Runtime runtime(42);
	runtime.load(schedule);
	runtime.load_properties(std::move(properties), schedule.properties->duration_tick,
							schedule.properties->property_file.string());
	ASSERT_NO_THROW(runtime.start(schedule.properties->duration_tick));

	constexpr std::chrono::microseconds SLICE{50000};
	constexpr std::chrono::milliseconds CHILD_BUDGET{5000};
	while (!runtime.failed() && runtime.properties_pending() > 0) {
		const uint64_t before = runtime.tick();
		ASSERT_LE(before, schedule.properties->duration_tick);
		const uint64_t ticks = std::min<uint64_t>(50, schedule.properties->duration_tick - before + 1);
		runtime.advance(std::chrono::microseconds(ticks * sim::TICK_DURATION.count()), 1.0, CHILD_BUDGET);
		ASSERT_GT(runtime.tick(), before);
	}

	std::string detail;
	for (const sim::PropertyResult& result : runtime.property_results()) {
		if (result.verdict != ltl::Verdict::TRUE) {
			detail += result.name + (result.verdict == ltl::Verdict::FALSE ? " was false" : " stayed pending");
			if (result.decisive_tick) detail += " at tick " + std::to_string(*result.decisive_tick);
			detail += "\n";
		}
	}
	EXPECT_FALSE(runtime.failed()) << detail;
	EXPECT_EQ(runtime.properties_pending(), 0u) << detail;
	runtime.stop();
	fs::remove(settings_path);
}
