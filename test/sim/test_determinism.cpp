#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

#include "../../src/module/module.h"
#include "../../src/settings.h"
#include "../../src/sim/runtime.h"
#include "../../src/world/world.h"

namespace fs = std::filesystem;

namespace {

	// The same inputs must always produce the same run, or the simulation is not debuggable.
	class DeterminismTest : public ::testing::Test {
	protected:
		static constexpr uint8_t FIRST  = 1;
		static constexpr uint8_t SECOND = 2;

		void SetUp() override {
			settings::install_test_defaults();
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}

			settings::set_setting<std::string>("application_path", FAKE_CHILD_PATH);
			settings::set_setting("simulation_speed", 1.0);
			settings::set_setting("seed", 1);
			settings::set_setting("network_fixed_delay_ms", 5);
			settings::set_setting("network_packet_loss_ppm", 0);
			settings::set_setting("network_very_late_ppm", 0);
			settings::set_setting("network_very_late_delay_ms", 1100);
			settings::set_setting("network_log_extra_delay_ms", 0);
			settings::set_setting("module_start_offset_mean_ms", 0.0);
			settings::set_setting("module_start_offset_sd_ms", 0.0);
			settings::set_setting("module_clock_drift_sd_ppm", 0.0);
			settings::set_setting("module_tick_skip_mean_ppm", 0.0);
			settings::set_setting("module_tick_skip_sd_ppm", 0.0);

			settings::ModuleSettings first{};
			first.active = true;
			first.left   = module::ModuleId{SECOND};
			settings::set_module_settings(module::ModuleId{FIRST}, first);

			settings::ModuleSettings second{};
			second.active = true;
			second.right  = module::ModuleId{FIRST};
			settings::set_module_settings(module::ModuleId{SECOND}, second);

			::setenv("FAKE_CHILD_SUBSCRIBE", std::to_string(EVENT_RFID_DETECT).c_str(), 1);
			::unsetenv("FAKE_CHILD_EXIT_AFTER");
			::unsetenv("FAKE_CHILD_RECORD");
		}

		void TearDown() override {
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
			::unsetenv("FAKE_CHILD_SUBSCRIBE");
		}

		// Runs one scenario end to end and returns the trace it produced.
		std::optional<world::Tub> final_tub_;

		std::string run(uint64_t seed, double speed, int ticks, uint32_t tick_batch = 1) {
			const fs::path trace_path = fs::temp_directory_path() /
										("quercus_trace_" + std::to_string(::getpid()) + "_" + std::to_string(seed) +
										 "_" + std::to_string(speed) + "_" + std::to_string(tick_batch) + ".ndjson");
			fs::remove(trace_path);

			{
				events::Schedule   schedule;
				events::TubArrival tub;
				tub.tick   = 0;
				tub.id     = 1;
				tub.module = module::ModuleId{FIRST};
				schedule.tubs.push_back(tub);

				events::TubArrival later;
				later.tick   = 20;
				later.id     = 2;
				later.module = module::ModuleId{SECOND};
				schedule.tubs.push_back(later);

				sim::Runtime runtime(seed, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, tick_batch);
				runtime.load(schedule);
				runtime.trace_to(trace_path.string(), {});
				runtime.start();

				// Gives the run a trajectory: both belts running and a servo out of its dead zone.
				for (const uint8_t id : {FIRST, SECOND}) {
					module::Module* hardware = runtime.world().module(module::ModuleId{id});
					hardware->set_servo_angle(module::ServoAngle{0});
					hardware->set_big_belt_speed(module::BeltSpeed{63.5});
					hardware->set_small_belt_speed(module::BeltSpeed{-41.25});
				}

				// Runs to a tick count, since speed changes how much time a frame buys.
				while (runtime.tick() < static_cast<uint64_t>(ticks)) {
					runtime.advance(std::chrono::microseconds(16000), speed, std::chrono::seconds(5));
				}
				// Captured before the runtime goes away: the world belongs to the run, not the process.
				if (const world::Tub* final_tub = runtime.world().tub(1)) {
					final_tub_ = *final_tub;
				} else {
					final_tub_.reset();
				}
				runtime.stop();
			}

			std::ifstream     file(trace_path);
			std::stringstream contents;
			contents << file.rdbuf();
			fs::remove(trace_path);
			return contents.str();
		}
	};

	// Drops the closing entry, since two runs paced differently stop on different ticks.
	std::string without_completion(const std::string& trace) {
		const std::size_t last = trace.find_last_of('\n', trace.size() - 2);
		return last == std::string::npos ? std::string{} : trace.substr(0, last + 1);
	}

} // namespace

TEST_F(DeterminismTest, TheSameSeedProducesAByteIdenticalTrace) {
	const std::string first  = run(42, 1.0, 400);
	const std::string second = run(42, 1.0, 400);

	EXPECT_FALSE(first.empty());
	EXPECT_EQ(first, second);
}

TEST_F(DeterminismTest, AnExplicitStrictBatchProducesTheDefaultTrace) {
	EXPECT_EQ(run(42, 1.0, 400), run(42, 1.0, 400, 1));
}

TEST_F(DeterminismTest, BatchedRunsProduceAByteIdenticalTrace) {
	EXPECT_EQ(run(42, 1.0, 400, 3), run(42, 1.0, 400, 3));
}

TEST_F(DeterminismTest, SpeedChangesPacingAndNotTheRun) {
	// A faster run overshoots and carries on further, so the slower run's trace must be its prefix.
	const std::string paced = without_completion(run(42, 1.0, 400));
	const std::string fast  = without_completion(run(42, 10.0, 400));

	ASSERT_FALSE(paced.empty());
	ASSERT_GE(fast.size(), paced.size());
	EXPECT_EQ(fast.compare(0, paced.size(), paced), 0);
}

TEST_F(DeterminismTest, ADifferentSeedProducesADifferentRun) {
	// If it did not, the seed would not be doing anything.
	EXPECT_NE(run(42, 1.0, 400), run(43, 1.0, 400));
}

TEST_F(DeterminismTest, TheWorldEndsInTheSameStateEveryTime) {
	run(42, 1.0, 400);
	ASSERT_TRUE(final_tub_.has_value());
	const int64_t first_position = final_tub_->position_um;
	const uint8_t first_module   = final_tub_->module.raw();

	run(42, 1.0, 400);
	ASSERT_TRUE(final_tub_.has_value());

	EXPECT_EQ(final_tub_->position_um, first_position);
	EXPECT_EQ(final_tub_->module.raw(), first_module);
}
