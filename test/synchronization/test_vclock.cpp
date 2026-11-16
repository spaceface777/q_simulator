#include <chrono>
#include <gtest/gtest.h>
#include <optional>
#include <thread>

#include "../../src/synchronization/virtual_clock.h"

using namespace std::chrono_literals;

namespace {

	using Clock = std::chrono::steady_clock;

	// A moment the clock read steady_clock, known only to lie between two reads of our own. Sleeps
	// may end late on a loaded machine, so the checks bound the clock by the real time that passed
	// rather than by the time requested.
	struct Moment {
		Clock::time_point first, last;
	};

	template <typename F> Moment around(F&& f) {
		auto first = Clock::now();
		f();
		return {first, Clock::now()};
	}

	// What tick() adds for a real interval: both of its conversions truncate to whole milliseconds.
	std::chrono::milliseconds scaled(Clock::duration real, double speed) {
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::duration_cast<std::chrono::milliseconds>(real) * speed);
	}

	// Checks the virtual time that a clock running at speed added between its readings at from and to.
	void expect_advance(std::chrono::milliseconds advance, Moment from, Moment to, double speed) {
		EXPECT_GE(advance, scaled(to.first - from.last, speed));
		EXPECT_LE(advance, scaled(to.last - from.first, speed));
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
	// Created inside around(), which records when the clock first reads the time
	std::optional<virtual_clock::VirtualClock> clock;

	auto created = around([&] { clock.emplace(); });

	// Sleep for at least 50ms real time
	std::this_thread::sleep_for(50ms);
	auto ticked = around([&] { clock->tick(); });

	// At 1x, virtual time advances by the real time that passed
	EXPECT_GE(clock->get_virtual_time(), 50ms);
	expect_advance(clock->get_virtual_time(), created, ticked, 1.0);
}

TEST(VirtualClockTest, SpeedMultiplier2X) {
	virtual_clock::VirtualClock clock;
	clock.set_simulation_speed(2.0);

	// Initial tick to establish baseline
	auto started    = around([&] { clock.tick(); });
	auto start_time = clock.get_virtual_time();

	// Sleep at least 50ms in real time
	std::this_thread::sleep_for(50ms);
	auto ticked = around([&] { clock.tick(); });

	auto virtual_elapsed = clock.get_virtual_time() - start_time;

	// At 2x speed, virtual time advances twice the real time that passed
	EXPECT_GE(virtual_elapsed, 100ms);
	expect_advance(virtual_elapsed, started, ticked, 2.0);
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
	// Created inside around(), which records when the clock first reads the time
	std::optional<virtual_clock::VirtualClock> clock;

	auto created = around([&] { clock.emplace(); });

	// Run at 1x for at least 30ms
	std::this_thread::sleep_for(30ms);
	auto first = around([&] { clock->tick(); });
	auto time1 = clock->get_virtual_time();
	EXPECT_GE(time1, 30ms);
	expect_advance(time1, created, first, 1.0);

	// Increase speed to 5x
	clock->set_simulation_speed(5.0);

	// Sleep at least another 30ms
	std::this_thread::sleep_for(30ms);
	auto second = around([&] { clock->tick(); });

	auto delta2 = clock->get_virtual_time() - time1;

	// At 5x, the second interval advances five times the real time that passed
	EXPECT_GE(delta2, 150ms);
	expect_advance(delta2, first, second, 5.0);
}

TEST(VirtualClockTest, InterfacePolymorphicUsage) {
	std::unique_ptr<virtual_clock::ISimulationClock> clock = std::make_unique<virtual_clock::VirtualClock>();

	clock->set_simulation_speed(3.0);
	EXPECT_DOUBLE_EQ(3.0, clock->simulation_speed());

	std::this_thread::sleep_for(30ms);
	clock->tick();

	EXPECT_GT(clock->get_virtual_time(), 0ms);
}