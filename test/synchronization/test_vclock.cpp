#include <chrono>
#include <gtest/gtest.h>
#include <thread>

#include "../../src/synchronization/virtual_clock.h"

using namespace std::chrono_literals;

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
	const auto                  before_start = std::chrono::steady_clock::now();
	virtual_clock::VirtualClock clock;
	const auto                  after_start = std::chrono::steady_clock::now();

	std::this_thread::sleep_for(50ms);
	const auto before_tick = std::chrono::steady_clock::now();
	clock.tick();
	const auto after_tick = std::chrono::steady_clock::now();

	// Bracket the clock's internal samples; the host can oversleep on a busy runner.
	EXPECT_GE(clock.get_virtual_time(),
			  std::chrono::duration_cast<std::chrono::milliseconds>(before_tick - after_start));
	EXPECT_LE(clock.get_virtual_time(),
			  std::chrono::duration_cast<std::chrono::milliseconds>(after_tick - before_start));
}

TEST(VirtualClockTest, SpeedMultiplier2X) {
	const auto                  before_start = std::chrono::steady_clock::now();
	virtual_clock::VirtualClock clock;
	clock.set_simulation_speed(2.0);
	const auto after_start = std::chrono::steady_clock::now();

	std::this_thread::sleep_for(50ms);
	const auto before_tick = std::chrono::steady_clock::now();
	clock.tick();
	const auto after_tick = std::chrono::steady_clock::now();

	// Check the multiplier against actual elapsed time, including scheduler delays.
	EXPECT_GE(clock.get_virtual_time(),
			  std::chrono::duration_cast<std::chrono::milliseconds>(before_tick - after_start) * 2);
	EXPECT_LE(clock.get_virtual_time(),
			  std::chrono::duration_cast<std::chrono::milliseconds>(after_tick - before_start) * 2);
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

	// Run at 1x for 30ms
	std::this_thread::sleep_for(30ms);
	clock.tick();
	auto time1 = clock.get_virtual_time();
	EXPECT_GE(time1, 20ms);

	// Increase speed to 5x
	clock.set_simulation_speed(5.0);

	// Sleep another 30ms
	std::this_thread::sleep_for(30ms);
	clock.tick();

	auto delta2 = clock.get_virtual_time() - time1;

	// 30ms @ 5x should be roughly ~150ms virtual elapsed
	EXPECT_GE(delta2, 120ms);
}

TEST(VirtualClockTest, InterfacePolymorphicUsage) {
	std::unique_ptr<virtual_clock::ISimulationClock> clock = std::make_unique<virtual_clock::VirtualClock>();

	clock->set_simulation_speed(3.0);
	EXPECT_DOUBLE_EQ(3.0, clock->simulation_speed());

	std::this_thread::sleep_for(30ms);
	clock->tick();

	EXPECT_GT(clock->get_virtual_time(), 0ms);
}
