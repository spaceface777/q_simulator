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
	virtual_clock::VirtualClock clock;

	// Sleep for 50ms real time
	std::this_thread::sleep_for(50ms);
	clock.tick();

	// Virtual time should have advanced by roughly 50ms (giving a small tolerance margin)
	EXPECT_GE(clock.get_virtual_time(), 40ms);
	EXPECT_LE(clock.get_virtual_time(), 100ms);
}

TEST(VirtualClockTest, SpeedMultiplier2X) {
	virtual_clock::VirtualClock clock;
	clock.set_simulation_speed(2.0);

	// Initial tick to establish baseline
	clock.tick();
	auto start_time = clock.get_virtual_time();

	// Sleep 50ms in real time
	std::this_thread::sleep_for(50ms);
	clock.tick();

	auto virtual_elapsed = clock.get_virtual_time() - start_time;

	// At 2x speed, 50ms real time should advance ~100ms virtual time
	EXPECT_GE(virtual_elapsed, 80ms);
	EXPECT_LE(virtual_elapsed, 150ms);
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