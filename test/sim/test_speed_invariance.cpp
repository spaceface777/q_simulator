#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <toml++/toml.hpp>
#include <unistd.h>

#include "../../src/event/events.h"
#include "../../src/settings.h"
#include "../../src/sim/runtime.h"
#include "../../src/world/calibration.h"
#include "../../src/world/world.h"

namespace fs = std::filesystem;

namespace {

	// Opt-in: real-time pacing makes a full pass take minutes per scenario.
	bool slow_tests_enabled() {
		const char* value = std::getenv("RUN_SLOW_TESTS");
		return value != nullptr && std::strcmp(value, "0") != 0 && std::strcmp(value, "") != 0;
	}

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
		std::string scenario;
		std::string layout;
		std::string applet;
	};

	// Mirrors the "Run it with" comment at the top of each file in scenarios/.
	const ScenarioCase kScenarios[] = {
		{"laser_alignment.toml", "one_module.toml", "laser_alignment"},
		{"module_movement.toml", "one_module.toml", "module_movement"},
		{"module_movement_idle.toml", "one_module.toml", "module_movement"},
		{"per_module_variation.toml", "default.toml", "module_movement"},
	};

	struct SpeedMode {
		std::string label;
		double      speed;
		bool        real_time_paced; // sleeps between advance() calls, as the GUI does
	};

	// 1000 is the smallest speed that saturates MAX_TICKS_PER_ADVANCE on a whole-tick elapsed value.
	const SpeedMode kSpeedModes[] = {
		{"0.3x", 0.3, true},
		{"1x", 1.0, true},
		{"10x", 10.0, true},
		{"max", 1000.0, false},
	};

	// parse_settings validates application_path against disk, so it must be baked in before parsing.
	std::string settings_with_binary(const fs::path& example, const std::string& binary) {
		toml::table table = toml::parse_file(example.string());
		table.insert_or_assign("application_path", binary);

		const fs::path temp =
			fs::temp_directory_path() / ("speed_test_settings_" + std::to_string(::getpid()) + ".toml");
		std::ofstream out(temp);
		out << table;
		return temp.string();
	}

	std::string scenario_name(const ::testing::TestParamInfo<ScenarioCase>& info) {
		const std::string& file = info.param.scenario;
		return file.substr(0, file.rfind(".toml"));
	}

	// An elapsed time that lands exactly on a tick boundary carries no budget over, so a caller can
	// step an exact number of ticks at any speed with no overshoot.
	std::chrono::microseconds elapsed_for_ticks(uint64_t ticks, double speed) {
		const int64_t target = static_cast<int64_t>(ticks) * sim::TICK_DURATION.count();
		auto product = [speed](int64_t elapsed) { return static_cast<int64_t>(static_cast<double>(elapsed) * speed); };

		int64_t elapsed = static_cast<int64_t>(std::llround(static_cast<double>(target) / speed));
		while (elapsed > 0 && product(elapsed) > target) {
			elapsed--;
		}
		while (product(elapsed) < target) {
			elapsed++;
		}
		return std::chrono::microseconds(elapsed);
	}

	struct RunResult {
		std::string trace;
		double      wall_seconds = 0.0;
		uint64_t    ticks        = 0;
		uint64_t    target_ticks = 0;
		bool        ok           = false;
	};

	double effective_speed(const RunResult& result) {
		return (static_cast<double>(result.ticks) * sim::TICK_DURATION.count() / 1e6) / result.wall_seconds;
	}

	// Runs one scenario at a fixed speed for a fixed tick count, writing an NDJSON trace to disk.
	void run_scenario_at_speed(const ScenarioCase& scenario, const SpeedMode& mode, const fs::path& settings_path,
							   const fs::path& trace_path, const fs::path& result_path) {
		const fs::path root(REPO_ROOT);
		settings::parse_settings(settings_path.string(), root / "layouts" / scenario.layout);

		const uint64_t         seed     = static_cast<uint64_t>(settings::get_setting<int>("seed"));
		const events::Schedule schedule = events::parse_events(root / "scenarios" / scenario.scenario, seed);
		// Legacy scenarios ended at their last assertion window, which Schedule::last_tick()
		// included. LTLf scenarios keep that execution bound in [scenario].duration instead.
		const uint64_t target_ticks =
			std::max(schedule.last_tick(), schedule.properties ? schedule.properties->duration_tick : uint64_t{0});

		world::configure_calibration_from_settings();
		world::set_calibration_seed(seed);

		sim::Runtime runtime(seed);
		runtime.load(schedule);
		runtime.trace_to(trace_path.string(), {});
		runtime.start();

		constexpr std::chrono::milliseconds CHILD_BUDGET{5000};

		// A bigger burst can overflow a child's inbound RPC ring before its main loop drains it.
		constexpr uint64_t MAX_BATCH = 50;

		// A paced mode uses cumulative deadlines. Tick work therefore consumes part of the period
		// instead of being added to a full sleep, and small sleep overshoots do not accumulate.
		const auto     loop_start       = std::chrono::steady_clock::now();
		const uint64_t paced_tick_start = runtime.tick();

		bool stalled = false;
		while (runtime.tick() < target_ticks && !runtime.failed()) {
			const uint64_t                  remaining = target_ticks - runtime.tick();
			const uint64_t                  batch     = mode.real_time_paced ? 1 : std::min(remaining, MAX_BATCH);
			const std::chrono::microseconds elapsed   = elapsed_for_ticks(batch, mode.speed);

			if (mode.real_time_paced) {
				const uint64_t ticks_due     = (runtime.tick() - paced_tick_start) + batch;
				const auto     wall_time_due = std::chrono::duration<double>(
					static_cast<double>(ticks_due * static_cast<uint64_t>(sim::TICK_DURATION.count())) /
					(mode.speed * 1e6));
				std::this_thread::sleep_until(
					loop_start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(wall_time_due));
			}

			const uint64_t before = runtime.tick();
			runtime.advance(elapsed, mode.speed, CHILD_BUDGET);
			if (runtime.tick() == before) {
				stalled = true;
				break;
			}
		}

		const double wall_seconds =
			std::chrono::duration<double>(std::chrono::steady_clock::now() - loop_start).count();

		const bool     ok    = !stalled && !runtime.failed() && runtime.tick() >= target_ticks;
		const uint64_t ticks = runtime.tick();

		runtime.stop();

		std::ofstream result(result_path);
		result << wall_seconds << '\n' << ticks << '\n' << (ok ? 1 : 0) << '\n' << target_ticks << '\n';
	}

	// A forked child, not a thread: settings:: and world::'s calibration seed are process-global.
	pid_t start_run(const ScenarioCase& scenario, const SpeedMode& mode, const fs::path& settings_path,
					const fs::path& trace_path, const fs::path& result_path) {
		const pid_t pid = ::fork();
		if (pid < 0) {
			throw std::runtime_error("fork() failed: " + std::string(std::strerror(errno)));
		}
		if (pid == 0) {
			try {
				run_scenario_at_speed(scenario, mode, settings_path, trace_path, result_path);
				::_exit(0);
			} catch (const std::exception& error) {
				std::cerr << "speed invariance child failed: " << error.what() << std::endl;
				::_exit(1);
			}
		}
		return pid;
	}

	RunResult finish_run(pid_t pid, const fs::path& trace_path, const fs::path& result_path, const std::string& tag) {
		int status = 0;
		::waitpid(pid, &status, 0);

		RunResult result;
		result.ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

		std::ifstream result_file(result_path);
		if (result_file) {
			result_file >> result.wall_seconds >> result.ticks;
			int ok_flag = 0;
			result_file >> ok_flag >> result.target_ticks;
			result.ok = result.ok && (ok_flag != 0);
		} else {
			result.ok = false;
		}

		std::ifstream     trace_file(trace_path);
		std::stringstream contents;
		contents << trace_file.rdbuf();
		result.trace = contents.str();

		fs::remove(trace_path);
		fs::remove(result_path);

		if (result.wall_seconds > 0.0) {
			std::cout << "[speed-invariance] " << tag << ": " << result.ticks << " ticks in " << result.wall_seconds
					  << "s (effective " << effective_speed(result) << "x)" << std::endl;
		}

		return result;
	}

	class SpeedInvarianceTest : public ::testing::TestWithParam<ScenarioCase> {};

} // namespace

// Speed paces how real time is spent, not what happens on a tick, so every trace must match.
TEST_P(SpeedInvarianceTest, ProducesTheSameTraceRegardlessOfSpeed) {
	if (!slow_tests_enabled()) {
		GTEST_SKIP() << "Set RUN_SLOW_TESTS=1 to run the speed invariance suite";
	}

	const ScenarioCase& scenario = GetParam();
	const std::string   binary   = applet_path(scenario.applet);
	if (binary.empty()) {
		GTEST_SKIP() << "No " << scenario.applet << " applet built; run (cd external/q_applets && ./q_sdk/qbuild sim)";
	}

	const fs::path    root(REPO_ROOT);
	const std::string settings_path = settings_with_binary(root / "settings_example.toml", binary);
	const std::string base          = scenario.scenario.substr(0, scenario.scenario.rfind(".toml"));

	auto path_for = [&](const std::string& label, const std::string& kind) {
		return fs::temp_directory_path() /
			   ("speed_test_" + base + "_" + label + "_" + kind + "_" + std::to_string(::getpid()));
	};

	// The paced modes are slow in wall time, so overlap them.
	const SpeedMode& slow          = kSpeedModes[0];
	const SpeedMode& normal        = kSpeedModes[1];
	const fs::path   slow_trace    = path_for(slow.label, "trace");
	const fs::path   slow_result   = path_for(slow.label, "result");
	const fs::path   normal_trace  = path_for(normal.label, "trace");
	const fs::path   normal_result = path_for(normal.label, "result");

	const pid_t slow_pid   = start_run(scenario, slow, settings_path, slow_trace, slow_result);
	const pid_t normal_pid = start_run(scenario, normal, settings_path, normal_trace, normal_result);

	const RunResult slow_run   = finish_run(slow_pid, slow_trace, slow_result, base + " @ " + slow.label);
	const RunResult normal_run = finish_run(normal_pid, normal_trace, normal_result, base + " @ " + normal.label);

	// The rest are cheap enough to run serially.
	const SpeedMode& fast        = kSpeedModes[2];
	const SpeedMode& max         = kSpeedModes[3];
	const fs::path   fast_trace  = path_for(fast.label, "trace");
	const fs::path   fast_result = path_for(fast.label, "result");
	const RunResult fast_run = finish_run(start_run(scenario, fast, settings_path, fast_trace, fast_result), fast_trace,
										  fast_result, base + " @ " + fast.label);

	const fs::path  max_trace  = path_for(max.label, "trace");
	const fs::path  max_result = path_for(max.label, "result");
	const RunResult max_run    = finish_run(start_run(scenario, max, settings_path, max_trace, max_result), max_trace,
											max_result, base + " @ " + max.label);

	fs::remove(settings_path);

	ASSERT_TRUE(slow_run.ok) << base << " @ " << slow.label << " did not reach its target tick";
	ASSERT_TRUE(normal_run.ok) << base << " @ " << normal.label << " did not reach its target tick";
	ASSERT_TRUE(fast_run.ok) << base << " @ " << fast.label << " did not reach its target tick";
	ASSERT_TRUE(max_run.ok) << base << " @ " << max.label << " did not reach its target tick";

	ASSERT_FALSE(slow_run.trace.empty());
	ASSERT_EQ(slow_run.target_ticks, normal_run.target_ticks);
	ASSERT_EQ(slow_run.target_ticks, fast_run.target_ticks);
	ASSERT_EQ(slow_run.target_ticks, max_run.target_ticks);

	// Every mode lands on the same final tick, so the whole file must match, not just a prefix.
	EXPECT_EQ(slow_run.trace, normal_run.trace) << slow.label << " vs " << normal.label << " diverged";
	EXPECT_EQ(slow_run.trace, fast_run.trace) << slow.label << " vs " << fast.label << " diverged";
	EXPECT_EQ(slow_run.trace, max_run.trace) << slow.label << " vs " << max.label << " diverged";

	auto expect_paced = [](const RunResult& run, const SpeedMode& mode) {
		EXPECT_NEAR(effective_speed(run), mode.speed, mode.speed * 0.05)
			<< mode.label << " pacing drifted by more than 5%";
	};

	expect_paced(slow_run, slow);
	expect_paced(normal_run, normal);

	// Only demand 10x when the same workload demonstrated enough unpaced throughput. This keeps
	// a genuinely overloaded machine from failing a pacing test while catching artificial caps.
	if (effective_speed(max_run) >= fast.speed) {
		expect_paced(fast_run, fast);
	}
}

INSTANTIATE_TEST_SUITE_P(AllScenarios, SpeedInvarianceTest, ::testing::ValuesIn(kScenarios), scenario_name);
