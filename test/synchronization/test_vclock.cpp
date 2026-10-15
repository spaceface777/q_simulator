#include <chrono>
#include <gtest/gtest.h>
#include <thread>

#include "../../src/synchronization/virtual_clock.h"

using namespace std::chrono_literals;

namespace {
	struct ClockSample {
		std::chrono::steady_clock::time_point before;
		std::chrono::steady_clock::time_point after;
		std::chrono::milliseconds             virtual_time;
	};

	ClockSample sample(virtual_clock::VirtualClock& clock) {
		const auto before = std::chrono::steady_clock::now();
		clock.tick();
		const auto after = std::chrono::steady_clock::now();
		return {before, after, clock.get_virtual_time()};
	}

	void expect_elapsed(const ClockSample& start, const ClockSample& end, int speed) {
		// Bracket the internal clock samples, including host scheduling and timer resolution.
		const auto elapsed = end.virtual_time - start.virtual_time;
		EXPECT_GE(elapsed, std::chrono::duration_cast<std::chrono::milliseconds>(end.before - start.after) * speed);
		EXPECT_LE(elapsed, std::chrono::duration_cast<std::chrono::milliseconds>(end.after - start.before) * speed);
	}
} // namespace

TEST(VirtualClockTest, DefaultInitialization) {
	virtual_clock::VirtualClock clock;

	// Initial virtual time should start at 0ms
	EXPECT_EQ(0ms, clock.get_virtual_time());
	// Default simulation speed should be 1.0
	EXPECT_DOUBLE_EQ(1.0, clock.simulation_speed());
}

TEST(VirtualClockTest, SetValidSimulationSpeed) {
	virtual_clock::VirtualClock clock;

	clock.set_simulation_speed(2.5);
	EXPECT_DOUBLE_EQ(2.5, clock.simulation_speed());

	clock.set_simulation_speed(0.0);
	EXPECT_DOUBLE_EQ(0.0, clock.simulation_speed());
}

TEST(VirtualClockTest, SetInvalidSimulationSpeedThrows) {
	virtual_clock::VirtualClock clock;

	// Speed < 0 should trigger util::require exception
	EXPECT_THROW(clock.set_simulation_speed(-0.1), std::invalid_argument);
	EXPECT_THROW(clock.set_simulation_speed(-10.0), std::invalid_argument);
}

TEST(VirtualClockTest, RealTimeTickAdvancement) {
	virtual_clock::VirtualClock clock;
	const auto                  start = sample(clock);
	std::this_thread::sleep_for(50ms);
	expect_elapsed(start, sample(clock), 1);
}

TEST(VirtualClockTest, SpeedMultiplier2X) {
	virtual_clock::VirtualClock clock;
	clock.set_simulation_speed(2.0);
	const auto start = sample(clock);
	std::this_thread::sleep_for(50ms);
	expect_elapsed(start, sample(clock), 2);
}

TEST(VirtualClockTest, PausedClockZeroSpeed) {
	virtual_clock::VirtualClock clock;
	clock.set_simulation_speed(0.0);

	std::this_thread::sleep_for(50ms);
	clock.tick();

	// At 0x speed, virtual time should not advance
	EXPECT_EQ(0ms, clock.get_virtual_time());
}

TEST(VirtualClockTest, DynamicSpeedChanges) {
	virtual_clock::VirtualClock clock;
	const auto                  start = sample(clock);
	std::this_thread::sleep_for(30ms);
	const auto first = sample(clock);
	expect_elapsed(start, first, 1);

	clock.set_simulation_speed(5.0);
	std::this_thread::sleep_for(30ms);
	expect_elapsed(first, sample(clock), 5);
}

TEST(VirtualClockTest, InterfacePolymorphicUsage) {
	std::unique_ptr<virtual_clock::ISimulationClock> clock = std::make_unique<virtual_clock::VirtualClock>();

	clock->set_simulation_speed(3.0);
	EXPECT_DOUBLE_EQ(3.0, clock->simulation_speed());

	std::this_thread::sleep_for(30ms);
	clock->tick();

	EXPECT_GT(clock->get_virtual_time(), 0ms);
}
