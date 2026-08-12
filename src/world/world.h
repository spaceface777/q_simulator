#ifndef WORLD_WORLD_H
#define WORLD_WORLD_H

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../defs.h"
#include "../module/module.h"
#include "../module/types.h"
#include "calibration.h"

namespace world {
	// Physical dimensions shared by the world model and renderer, expressed against the full
	// corresponding belt length. These preserve the established 20-pixel GUI tub/reader geometry.
	inline constexpr double TUB_RADIUS_BIG_BELT_FRACTION  = 5.0 / 63.0;
	inline constexpr double RFID_HALF_SMALL_BELT_FRACTION = 4.0 / 21.0;

	// The physical world: where every tub is, and what that means for the sensors. Positions are
	// integer micrometres and time is an integer tick, so a run's outcome never depends on
	// floating-point rounding order.

	// Which of a module's two conveyors a tub is riding. The big belt is one axis: 0 at the right
	// endpoint, endpoint_to_junction_um at the junction, twice that at the left endpoint. The small
	// belt runs from the junction (0) to its outer endpoint.
	enum class Belt : uint8_t {
		BIG,
		SMALL,
	};

	// Where a tub sits relative to a module's fixed points.
	enum class Position : uint8_t {
		ENTRY,       // At the RFID reader.
		CENTER,      // On the central junction.
		LEFT_LASER,  // At the left laser.
		RIGHT_LASER, // At the right laser.
	};

	struct Tub {
		uint8_t          id = 0;
		module::ModuleId module;
		Belt             belt = Belt::BIG;

		int64_t position_um = 0;

		// Sub-micrometre remainder of the speed integrator, so slow belts accumulate travel exactly
		// instead of rounding to a standstill.
		int64_t carry = 0;

		RfidUid     uid{};
		RfidPayload payload{};
	};

	// How a tub is currently moving. Derived rather than stored, so reversing a belt immediately
	// reverses what is reported.
	struct Movement {
		module::ModuleId module;
		Position         from     = Position::CENTER;
		Position         to       = Position::CENTER;
		double           progress = 0.0;
		bool             moving   = false;
	};

	class State {
	private:
		std::map<uint8_t, module::Module> modules_;
		std::map<uint8_t, Tub>            tubs_;

		// Decides how a tub sitting on the junction proceeds. Returns whether it may move this tick.
		bool resolve_junction(Tub& tub, int64_t leg_length_um);

		// Moves a tub along its belt by a signed step, clamping at the junction and attempting a
		// hand-off at an outward endpoint.
		void move(Tub& tub, int64_t leg_length_um, int64_t step_um);

		// Attempts to move a tub into the neighbouring module it is leaving towards.
		void hand_off(Tub& tub, int64_t leg_length_um);

		[[nodiscard]] bool       can_receive(module::ModuleId id, Position position) const;
		[[nodiscard]] const Tub* tub_at_reader(module::ModuleId id) const;

		// Writes the sensor consequences of the current tub positions into the modules.
		void update_sensors();

	public:
		void add_module(module::ModuleId id);

		[[nodiscard]] module::Module*       module(module::ModuleId id);
		[[nodiscard]] const module::Module* module(module::ModuleId id) const;

		// Throws std::invalid_argument if the module does not exist or the tub id is taken.
		void place(uint8_t id, module::ModuleId module, Position position, const RfidUid& uid,
				   const RfidPayload& payload);

		void place(uint8_t id, module::ModuleId module, Position position, const RfidPayload& payload) {
			place(id, module, position, RfidUid{}, payload);
		}

		void remove(uint8_t id);

		// Persists an RFID write into the tub whose centre is within this module's reader footprint. The
		// module's sensor buffer is only a projection of world state and is refreshed every tick,
		// so writes must update the tub rather than that transient projection alone.
		[[nodiscard]] bool write_rfid_page(module::ModuleId id, std::size_t page, const RfidPage& value);

		// Advances every tub by one tick and republishes the sensor state that follows. Tubs advance
		// in ascending id order so contention for the same endpoint always resolves the same way.
		void advance_tick();

		// Tubs are kept by ascending id. Readers can inspect the immutable range without copying the
		// RFID payloads stored in each tub.
		[[nodiscard]] const std::map<uint8_t, Tub>& tubs() const noexcept { return tubs_; }

		// The returned pointer remains valid until that tub is removed, the state is cleared, or the
		// state is destroyed.
		[[nodiscard]] const Tub* tub(uint8_t id) const;

		// A tub counts as being at a laser when its physical extent intersects the beam; it counts as
		// being at entry when its centre lies within the RFID footprint.
		[[nodiscard]] std::optional<Position> position_of(uint8_t id) const;
		[[nodiscard]] std::optional<Position> position_of(const Tub& tub) const;

		[[nodiscard]] Movement movement_of(uint8_t id) const;

		void clear();
	};

	// Reads the calibration tunables out of the parsed settings and installs them. Throws
	// std::invalid_argument if the settings have not been parsed.
	void configure_calibration_from_settings();

	// Commands a module's servo to an angle, taking the time its arm takes to get there.
	// module::Module::set_servo_angle places an arm without travel and is for setup, not driving.
	void command_servo(module::Module& module, module::ServoAngle target);

	[[nodiscard]] std::string to_string(Position position);

	[[nodiscard]] std::optional<Position> position_from_string(const std::string& text);

	// What a module is drawing right now, and at what voltage.
	struct Electrics {
		double current_a = 0.0;
		double voltage_v = 0.0;
		double power_w   = 0.0;
	};

	// Power is the product of current and voltage, not a third measurement.
	[[nodiscard]] Electrics electrics_of(const module::Module& module, const Calibration& calibration);

	// The encoder count for total belt travel. Only ever rises: running a belt backwards turns it
	// as much as running it forwards does.
	[[nodiscard]] int64_t encoder_count(int64_t travel_um, const Calibration& calibration);

	// Counts per second the encoder is turning out, unsigned, read off the belt's curve so it
	// reflects the curve's nonlinearity rather than a plain scale from a maximum.
	[[nodiscard]] double encoder_frequency_hz(double speed_percent, const BeltCurve& curve,
											  const Calibration& calibration);

	// Integrates one tick of signed belt travel at a known rate. `carry` is the fractional
	// remainder, updated in place.
	[[nodiscard]] int64_t belt_step_um(double um_per_s, int64_t& carry);

	// Integrates one tick of signed belt travel from a command, via the belt's speed curve.
	[[nodiscard]] int64_t belt_step_um(double speed_percent, const BeltCurve& curve, int64_t& carry);

	// Physical half extents used by the sensor occupancy model, derived from the GUI design geometry.
	[[nodiscard]] int64_t tub_radius_um(const Calibration& calibration);
	[[nodiscard]] int64_t rfid_half_length_um(const Calibration& calibration);

} // namespace world

#endif // WORLD_WORLD_H
