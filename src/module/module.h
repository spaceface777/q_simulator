#ifndef MODULE_H
#define MODULE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "../defs.h"
#include "types.h"

namespace module {

	// The simulated hardware of one module: actuator settings and the sensor readings that follow.
	class Module {
	private:
		ModuleId id_;

		BeltSpeed  big_speed_;
		BeltSpeed  small_speed_;
		ServoAngle servo_degrees_;
		LedColor   led_color_;

		// Where the servo arm physically is; servo_degrees_ above is only the commanded angle.
		double  servo_from_degrees_  = 0.0;
		int64_t servo_travel_ticks_  = 0;
		int64_t servo_elapsed_ticks_ = 0;

		uint64_t servo_movements_     = 0;
		bool     left_laser_blocked_  = false;
		bool     right_laser_blocked_ = false;
		bool     rfid_active_         = false;

		// "enabled" is application-set power state; "active" is the world's beam-broken state.
		bool left_laser_enabled_  = false;
		bool right_laser_enabled_ = false;

		RfidUid     rfid_uid_{};
		RfidPayload rfid_data_{};

		// Belt travel, not tub travel, and distance, not displacement: it only ever counts up.
		int64_t big_travel_um_      = 0;
		int64_t small_travel_um_    = 0;
		int64_t big_travel_carry_   = 0;
		int64_t small_travel_carry_ = 0;

	public:
		Module(uint8_t id) { id_ = ModuleId{id}; }

		ModuleId id() const noexcept { return id_; }

		BeltSpeed big_belt_speed() const noexcept { return big_speed_; }

		BeltSpeed small_belt_speed() const noexcept { return small_speed_; }

		ServoAngle servo_angle() const noexcept { return servo_degrees_; }

		LedColor led_color() const noexcept { return led_color_; }

		// Micrometres, since the run started.
		int64_t big_belt_travel_um() const noexcept { return big_travel_um_; }

		int64_t small_belt_travel_um() const noexcept { return small_travel_um_; }

		// Only magnitude is kept: the encoder counts turning, not direction.
		void advance_belt_travel(int64_t big_step_um, int64_t small_step_um) noexcept {
			big_travel_um_ += big_step_um < 0 ? -big_step_um : big_step_um;
			small_travel_um_ += small_step_um < 0 ? -small_step_um : small_step_um;
		}

		void reset_belt_travel() noexcept {
			big_travel_um_      = 0;
			small_travel_um_    = 0;
			big_travel_carry_   = 0;
			small_travel_carry_ = 0;
		}

		// Carried between ticks so a slow belt accumulates travel exactly instead of rounding to zero.
		int64_t& big_belt_travel_carry() noexcept { return big_travel_carry_; }

		int64_t& small_belt_travel_carry() noexcept { return small_travel_carry_; }

		void set_big_belt_speed(BeltSpeed speed) noexcept { big_speed_ = speed; }

		void set_small_belt_speed(BeltSpeed speed) noexcept { small_speed_ = speed; }

		// Instant, with no travel. For test setup; use command_servo_angle to model a real command.
		void set_servo_angle(ServoAngle degrees) noexcept {
			servo_degrees_       = degrees;
			servo_from_degrees_  = degrees.raw();
			servo_travel_ticks_  = 0;
			servo_elapsed_ticks_ = 0;
		}

		// Starts from the arm's actual position, so a mid-movement command redirects it, not resets it.
		void command_servo_angle(ServoAngle degrees, int64_t travel_ticks) noexcept {
			const double current = servo_actual_angle().raw();
			servo_degrees_       = degrees;
			servo_movements_++;

			if (travel_ticks <= 0) {
				servo_from_degrees_  = degrees.raw();
				servo_travel_ticks_  = 0;
				servo_elapsed_ticks_ = 0;
				return;
			}

			servo_from_degrees_  = current;
			servo_travel_ticks_  = travel_ticks;
			servo_elapsed_ticks_ = 0;
		}

		// Interpolated along current travel; equal to the commanded angle when not moving.
		[[nodiscard]] ServoAngle servo_actual_angle() const noexcept {
			if (servo_travel_ticks_ <= 0 || servo_elapsed_ticks_ >= servo_travel_ticks_) {
				return servo_degrees_;
			}
			const double progress =
				static_cast<double>(servo_elapsed_ticks_) / static_cast<double>(servo_travel_ticks_);
			return ServoAngle{servo_from_degrees_ + progress * (servo_degrees_.raw() - servo_from_degrees_)};
		}

		[[nodiscard]] bool is_servo_moving() const noexcept {
			return servo_travel_ticks_ > 0 && servo_elapsed_ticks_ < servo_travel_ticks_;
		}

		[[nodiscard]] uint64_t servo_movement_count() const noexcept { return servo_movements_; }

		void advance_servo_travel() noexcept {
			if (servo_travel_ticks_ <= 0) {
				return;
			}
			if (servo_elapsed_ticks_ < servo_travel_ticks_) {
				servo_elapsed_ticks_++;
			}
			if (servo_elapsed_ticks_ >= servo_travel_ticks_) {
				servo_from_degrees_  = servo_degrees_.raw();
				servo_travel_ticks_  = 0;
				servo_elapsed_ticks_ = 0;
			}
		}

		void set_led_color(LedColor color) noexcept { led_color_ = color; }

		bool is_left_laser_blocked() const noexcept { return left_laser_blocked_; }

		void set_left_laser_blocked(bool blocked) noexcept { left_laser_blocked_ = blocked; }

		bool is_right_laser_blocked() const noexcept { return right_laser_blocked_; }

		void set_right_laser_blocked(bool blocked) noexcept { right_laser_blocked_ = blocked; }

		bool is_rfid_active() const noexcept { return rfid_active_; }

		void set_rfid_active(bool active) noexcept { rfid_active_ = active; }

		bool is_left_laser_enabled() const noexcept { return left_laser_enabled_; }

		bool is_right_laser_enabled() const noexcept { return right_laser_enabled_; }

		void set_left_laser_enabled(bool enabled) noexcept { left_laser_enabled_ = enabled; }

		void set_right_laser_enabled(bool enabled) noexcept { right_laser_enabled_ = enabled; }

		// An unpowered laser never reports a clear beam.
		[[nodiscard]] bool is_laser_clear(bool left) const noexcept {
			if (left) {
				return left_laser_enabled_ && !left_laser_blocked_;
			}
			return right_laser_enabled_ && !right_laser_blocked_;
		}

		[[nodiscard]] const RfidUid& rfid_uid() const noexcept { return rfid_uid_; }

		[[nodiscard]] RfidPage rfid_page(std::size_t page) const { return rfid_data_.at(page); }

		void set_rfid_tag(const RfidUid& uid, const RfidPayload& data) {
			rfid_uid_    = uid;
			rfid_data_   = data;
			rfid_active_ = true;
		}

		// Returns false if there is no tag or the page index is out of range.
		bool write_rfid_page(std::size_t page, const RfidPage& value) {
			if (!rfid_active_ || page >= RFID_PAYLOAD_PAGE_COUNT) {
				return false;
			}
			rfid_data_[page] = value;
			return true;
		}

		void clear_rfid_tag() {
			rfid_uid_    = {};
			rfid_data_   = {};
			rfid_active_ = false;
		}
	};

} // namespace module

#endif // MODULE_H
