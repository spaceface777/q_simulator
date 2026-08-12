#ifndef CLOCK_H
#define CLOCK_H

#include <chrono>

#include "../util.h"

namespace virtual_clock {

	class ISimulationClock {
	public:
		virtual ~ISimulationClock() = default;

		virtual void                      tick() noexcept                    = 0;
		virtual std::chrono::milliseconds get_virtual_time() const noexcept  = 0;
		virtual double                    simulation_speed() const noexcept  = 0;
		virtual void                      set_simulation_speed(double speed) = 0;
	};

	class VirtualClock : public ISimulationClock {
	private:
		std::chrono::milliseconds                          _virtual_time = std::chrono::milliseconds(0);
		std::chrono::time_point<std::chrono::steady_clock> _last_time_point;
		double                                             _simulation_speed = 1.0;

	public:
		VirtualClock() noexcept : _last_time_point(std::chrono::steady_clock::now()) {}

		// Call at least as often as simulation speed changes, or a speed change is applied late.
		void tick() noexcept override {
			auto now               = std::chrono::steady_clock::now();
			auto real_time_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - _last_time_point);
			auto virtual_delta =
				std::chrono::duration_cast<std::chrono::milliseconds>(real_time_elapsed * _simulation_speed);
			_virtual_time += virtual_delta;
			_last_time_point = now;
		}

		// Virtual time comes from the engine's tick count, not from this clock's own estimate.
		void set_virtual_time(std::chrono::milliseconds time) noexcept { _virtual_time = time; }

		std::chrono::milliseconds get_virtual_time() const noexcept override { return _virtual_time; }

		double simulation_speed() const noexcept override { return _simulation_speed; }

		// Throws if speed < 0.
		void set_simulation_speed(double speed) override {
			util::require(speed >= 0, "Invalid simulation speed in settings.yml");

			_simulation_speed = speed;
		}
	};

} // namespace virtual_clock

#endif