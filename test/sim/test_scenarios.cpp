#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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

	// Skips unless a real applet binary is built; see external/q_applets, built via
	// `(cd external/q_applets && ./q_sdk/qbuild sim)`.
	std::string applet_path(const std::string& applet) {
		const fs::path path = fs::path(REPO_ROOT) / "external" / "q_applets" / "out" / (applet + ".exe");
		if (!fs::exists(path)) {
			return "";
		}
		return path.string();
	}

	struct ScenarioCase {
		std::string           scenario; // file under scenarios/
		std::string           layout;   // file under layouts/, matching the -l the scenario's header documents
		std::string           applet;   // applet name under external/q_applets/out
		uint32_t              tick_batch;
		std::optional<double> tick_skip_ppm;
	};

	// Records the expected self-contained configuration of each bundled scenario.
	const std::vector<ScenarioCase> kScenarios = [] {
		const ScenarioCase base[] = {
			{"hardware_test.toml", "one_module.toml", "hardware_test", 1, std::nullopt},
			{"hardware_test_blocked_beam.toml", "one_module.toml", "hardware_test", 1, std::nullopt},
			{"hardware_test_tag_at_startup.toml", "one_module.toml", "hardware_test", 1, std::nullopt},
			{"laser_alignment.toml", "one_module.toml", "laser_alignment", 1, std::nullopt},
			{"module_movement.toml", "one_module.toml", "module_movement", 1, std::nullopt},
			{"module_movement_idle.toml", "one_module.toml", "module_movement", 1, std::nullopt},
			{"per_module_variation.toml", "default.toml", "module_movement", 1, std::nullopt},
		};
		std::vector<ScenarioCase> result;
		for (const ScenarioCase& scenario : base) {
			for (const uint32_t tick_batch : {1u, 5u, 15u}) {
				result.push_back({scenario.scenario, scenario.layout, scenario.applet, tick_batch, std::nullopt});
			}
			result.push_back({scenario.scenario, scenario.layout, scenario.applet, 15, 200'000.0});
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
		std::string name = file.substr(0, file.rfind(".toml")) + "_batch_" + std::to_string(info.param.tick_batch);
		if (info.param.tick_skip_ppm) name += "_tick_loss_20_percent";
		return name;
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
		GTEST_SKIP() << "No " << scenario.applet << " applet built; run (cd external/q_applets && ./q_sdk/qbuild sim)";
	}

	const fs::path              root(REPO_ROOT);
	const fs::path              scenario_path = root / "scenarios" / scenario.scenario;
	const events::ScenarioFiles files         = events::parse_scenario_files(scenario_path);
	ASSERT_TRUE(files.layout.has_value());
	ASSERT_TRUE(files.binary.has_value());
	EXPECT_EQ(*files.layout, root / "layouts" / scenario.layout);
	EXPECT_EQ(*files.binary, root / "external" / "q_applets" / "out" / (scenario.applet + ".exe"));

	const std::string settings_path = settings_with_binary(root / "settings_example.toml", files.binary->string());
	settings::parse_settings(settings_path, *files.layout);
	if (scenario.tick_skip_ppm) {
		settings::set_setting("module_tick_skip_mean_ppm", *scenario.tick_skip_ppm);
		settings::set_setting("module_tick_skip_sd_ppm", 0.0);
	}

	const uint64_t         seed     = static_cast<uint64_t>(settings::get_setting<int>("seed"));
	const events::Schedule schedule = events::parse_events(scenario_path, seed);
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

TEST(ScenarioExactRunahead, MatchesStrictTraceBytesAcrossEventsSeedsAndManyChildren) {
	struct ExactCase {
		const char* scenario;
		const char* layout;
		const char* applet;
		uint64_t    seed;
	};

	const ExactCase cases[] = {
		{"hardware_test_blocked_beam.toml", "one_module.toml", "hardware_test", 7},
		{"module_movement.toml", "one_module.toml", "module_movement", 42},
		{"per_module_variation.toml", "default.toml", "module_movement", 1},
		{"exact_runahead_idle.toml", "default.toml", "net_test", 17},
		{"exact_runahead_idle.toml", "default.toml", "runahead_bench", 1},
	};
	const fs::path root(REPO_ROOT);

	for (const ExactCase& test_case : cases) {
		const std::string binary = applet_path(test_case.applet);
		if (binary.empty()) {
			GTEST_SKIP() << "No " << test_case.applet << " applet built; run qbuild sim";
		}

		const fs::path scenario_path = root / "scenarios" / test_case.scenario;
		const fs::path layout_path   = root / "layouts" / test_case.layout;
		const auto     execute       = [&](uint32_t runahead_cap, const std::string& mode) {
			const std::string settings_path = settings_with_binary(root / "settings_example.toml", binary);
			settings::parse_settings(settings_path, layout_path);
			settings::set_setting("seed", static_cast<int>(test_case.seed));
			const events::Schedule schedule = events::parse_events(scenario_path, test_case.seed);
			if (!schedule.properties) {
				ADD_FAILURE() << test_case.scenario << " has no properties";
				return std::string{};
			}

			world::configure_calibration_from_settings();
			world::set_calibration_seed(test_case.seed);
			ltl::File      properties = parse_properties(schedule.properties->property_file);
			const fs::path trace_path =
				fs::temp_directory_path() /
				("quercus_exact_scenario_" + std::to_string(::getpid()) + "_" + test_case.scenario + "_" + mode);
			fs::remove(trace_path);

			sim::Runtime runtime(test_case.seed, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, 1, runahead_cap);
			runtime.load(schedule);
			runtime.load_properties(std::move(properties), schedule.properties->duration_tick,
									schedule.properties->property_file.string());
			runtime.trace_to(trace_path.string(), {});
			runtime.start(schedule.properties->duration_tick);
			while (!runtime.failed() && runtime.properties_pending() > 0) {
				const uint64_t before     = runtime.tick();
				const uint64_t ticks_left = schedule.properties->duration_tick - before + 1;
				const uint64_t ticks      = std::min<uint64_t>(50, ticks_left);
				runtime.advance(std::chrono::microseconds(ticks * sim::TICK_DURATION.count()), 1.0,
								std::chrono::seconds(5));
				if (runtime.tick() == before) {
					ADD_FAILURE() << test_case.scenario << " stopped at tick " << before;
					break;
				}
			}
			runtime.stop();
			fs::remove(settings_path);

			std::ifstream      input(trace_path, std::ios::binary);
			std::ostringstream contents;
			contents << input.rdbuf();
			fs::remove(trace_path);
			return contents.str();
		};

		const std::string strict = execute(0, "strict");
		const std::string exact  = execute(sim::DEFAULT_RUNAHEAD_TICKS, "exact");
		EXPECT_FALSE(strict.empty()) << test_case.scenario;
		EXPECT_EQ(exact, strict) << test_case.scenario << " changed strict trace bytes";
	}
}

TEST(ScenarioSeedCorpus, ModuleMovementPassesTheStableV01SeedsWithoutSpecEdits) {
	const std::string binary = applet_path("module_movement");
	if (binary.empty()) {
		GTEST_SKIP() << "No module_movement applet built; run (cd external/q_applets && ./q_sdk/qbuild sim)";
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
	for (const uint32_t batch : {1u, 5u, 15u}) {
		ltl::File properties = parse_properties(schedule.properties->property_file);
		world::configure_calibration_from_settings();
		world::set_calibration_seed(42);

		sim::Runtime runtime(42, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, batch);
		runtime.load(schedule);
		runtime.load_properties(std::move(properties), schedule.properties->duration_tick,
								schedule.properties->property_file.string());
		runtime.start(schedule.properties->duration_tick);

		constexpr std::chrono::microseconds SLICE{50000};
		constexpr std::chrono::milliseconds CHILD_BUDGET{5000};
		while (!runtime.failed() && runtime.properties_pending() > 0) {
			const uint64_t before = runtime.tick();
			ASSERT_LE(before, schedule.properties->duration_tick) << "batch " << batch;
			const uint64_t ticks_left = schedule.properties->duration_tick - before + 1;
			uint64_t       ticks      = std::min<uint64_t>(50, ticks_left);
			if (ticks % batch != 0) ticks += batch - (ticks % batch);
			runtime.advance(std::chrono::microseconds(ticks * sim::TICK_DURATION.count()), 1.0, CHILD_BUDGET);
			ASSERT_GT(runtime.tick(), before) << "batch " << batch;
		}

		std::string detail;
		for (const sim::PropertyResult& result : runtime.property_results()) {
			if (result.verdict != ltl::Verdict::TRUE) {
				detail += result.name + (result.verdict == ltl::Verdict::FALSE ? " was false" : " stayed pending");
				if (result.decisive_tick) detail += " at tick " + std::to_string(*result.decisive_tick);
				detail += "\n";
			}
		}
		EXPECT_FALSE(runtime.failed()) << "batch " << batch << "\n" << detail;
		EXPECT_EQ(runtime.properties_pending(), 0u) << "batch " << batch << "\n" << detail;
		runtime.stop();
	}
	fs::remove(settings_path);
}

TEST(ScenarioBatchTrace, RealScenarioDiffersOnlyAtDocumentedBatchHandoffBoundaries) {
	const std::string binary = applet_path("module_movement");
	if (binary.empty()) {
		GTEST_SKIP() << "No module_movement applet built; run qbuild sim module_movement";
	}

	const fs::path    root(REPO_ROOT);
	const std::string settings_path = settings_with_binary(root / "settings_example.toml", binary);
	settings::parse_settings(settings_path, root / "layouts" / "one_module.toml");
	const uint64_t seed = static_cast<uint64_t>(settings::get_setting<int>("seed"));

	const events::Schedule schedule = events::parse_events(root / "scenarios" / "module_movement.toml", seed);
	ASSERT_TRUE(schedule.properties.has_value());

	struct CapturedRun {
		std::vector<std::string> batch_independent_records;
		std::vector<std::string> skipped_tick_records;
		std::vector<std::string> property_verdict_records;
	};

	const auto execute = [&](uint32_t batch) {
		const fs::path trace_path =
			fs::temp_directory_path() /
			("quercus_real_skip_trace_" + std::to_string(::getpid()) + "_" + std::to_string(batch) + ".ndjson");
		fs::remove(trace_path);

		ltl::File properties = parse_properties(schedule.properties->property_file);
		world::configure_calibration_from_settings();
		world::set_calibration_seed(seed);

		sim::Runtime runtime(seed, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, batch);
		runtime.load(schedule);
		runtime.load_properties(std::move(properties), schedule.properties->duration_tick,
								schedule.properties->property_file.string());
		runtime.trace_to(trace_path.string(), {});
		runtime.start(schedule.properties->duration_tick);

		constexpr std::chrono::microseconds SLICE{50000};
		constexpr std::chrono::milliseconds CHILD_BUDGET{5000};
		while (!runtime.failed() && runtime.properties_pending() > 0) {
			const uint64_t before = runtime.tick();
			if (before > schedule.properties->duration_tick) {
				ADD_FAILURE() << "batch " << batch << " passed the property endpoint";
				break;
			}
			const uint64_t ticks_left = schedule.properties->duration_tick - before + 1;
			uint64_t       ticks      = std::min<uint64_t>(50, ticks_left);
			if (ticks % batch != 0) ticks += batch - (ticks % batch);
			runtime.advance(std::chrono::microseconds(ticks * sim::TICK_DURATION.count()), 1.0, CHILD_BUDGET);
			if (runtime.tick() == before) {
				ADD_FAILURE() << "batch " << batch << " stopped making progress at tick " << before;
				break;
			}
		}

		std::string detail;
		for (const sim::PropertyResult& result : runtime.property_results()) {
			if (result.verdict != ltl::Verdict::TRUE) {
				detail += result.name + (result.verdict == ltl::Verdict::FALSE ? " was false" : " stayed pending");
				if (result.decisive_tick) detail += " at tick " + std::to_string(*result.decisive_tick);
				detail += "\n";
			}
		}
		EXPECT_FALSE(runtime.failed()) << "batch " << batch << "\n" << detail;
		EXPECT_EQ(runtime.properties_pending(), 0u) << "batch " << batch << "\n" << detail;
		runtime.stop();

		CapturedRun   captured;
		std::ifstream trace(trace_path);
		std::string   line;
		while (std::getline(trace, line)) {
			const std::string marker = "\"type\":\"";
			const std::size_t begin  = line.find(marker);
			const std::size_t end    = begin == std::string::npos ? begin : line.find('"', begin + marker.size());
			if (begin == std::string::npos || end == std::string::npos) {
				ADD_FAILURE() << "malformed trace record: " << line;
				continue;
			}
			const std::string type = line.substr(begin + marker.size(), end - begin - marker.size());
			if (type == "module_tick_skipped") captured.skipped_tick_records.push_back(line);
			if (type == "property_verdict") captured.property_verdict_records.push_back(line);

			// These records can move when requests and responses wait for the next batch handoff.
			// Everything else describes batch-independent clock/skip/completion semantics and must
			// remain byte-identical to strict mode.
			const bool handoff_dependent = type == "run_start" || type == "rpc_event" || type == "rpc_request" ||
										   type == "rpc_response" || type == "tub" || type == "property_activation" ||
										   type == "property_verdict";
			if (!handoff_dependent) captured.batch_independent_records.push_back(line);
		}
		fs::remove(trace_path);
		return captured;
	};

	const CapturedRun strict   = execute(1);
	const CapturedRun batch_5  = execute(5);
	const CapturedRun batch_15 = execute(15);
	ASSERT_FALSE(strict.skipped_tick_records.empty());
	EXPECT_EQ(strict.skipped_tick_records.size(), 45u);
	EXPECT_EQ(batch_5.skipped_tick_records, strict.skipped_tick_records)
		<< "5x batching changed the exact global/local skipped-tick sequence";
	EXPECT_EQ(batch_15.skipped_tick_records, strict.skipped_tick_records)
		<< "15x batching changed the exact global/local skipped-tick sequence";
	EXPECT_EQ(batch_5.batch_independent_records, strict.batch_independent_records)
		<< "5x batching changed trace data outside documented batch-boundary handoff timing";
	EXPECT_EQ(batch_15.batch_independent_records, strict.batch_independent_records)
		<< "15x batching changed trace data outside documented batch-boundary handoff timing";
	ASSERT_EQ(batch_5.property_verdict_records.size(), strict.property_verdict_records.size());
	ASSERT_EQ(batch_15.property_verdict_records.size(), strict.property_verdict_records.size());
	for (const CapturedRun* run : {&strict, &batch_5, &batch_15}) {
		for (const std::string& verdict : run->property_verdict_records) {
			EXPECT_NE(verdict.find("\"verdict\":\"true\""), std::string::npos) << verdict;
		}
	}
	fs::remove(settings_path);
}
