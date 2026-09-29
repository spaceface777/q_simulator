#ifndef LTL_OBSERVATION_H
#define LTL_OBSERVATION_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <vector>

#include "../module/types.h"
#include "../world/world.h"
#include "ast.h"

namespace ltl {

	enum class ModuleField : uint8_t {
		BIG_BELT_SPEED,
		SMALL_BELT_SPEED,
		SERVO_ANGLE,
		SERVO_PHYSICAL_ANGLE,
		LED_COLOR,
		LEFT_LASER_ENABLED,
		RIGHT_LASER_ENABLED,
		LEFT_LASER_BLOCKED,
		RIGHT_LASER_BLOCKED,
		RFID_ACTIVE,
	};

	struct ModuleSnapshot {
		uint16_t id                   = 0;
		double   big_belt_speed       = 0.0;
		double   small_belt_speed     = 0.0;
		double   servo_angle          = 0.0;
		double   servo_physical_angle = 0.0;
		uint32_t led_color            = 0;
		bool     left_laser_enabled   = false;
		bool     right_laser_enabled  = false;
		bool     left_laser_blocked   = false;
		bool     right_laser_blocked  = false;
		bool     rfid_active          = false;
		int64_t  leg_length_um        = 0;
		int64_t  tub_radius_um        = 0;

		bool operator==(const ModuleSnapshot&) const = default;
	};

	struct TubSnapshot {
		uint16_t                       id          = 0;
		uint16_t                       module      = 0;
		world::Belt                    belt        = world::Belt::BIG;
		int64_t                        position_um = 0;
		std::optional<world::Position> position;
		std::vector<uint8_t>           uid;
		std::vector<uint8_t>           data;
		// Source of a physical module transition on this observation, otherwise zero.
		uint16_t                      previous_module = 0;

		bool operator==(const TubSnapshot&) const = default;
	};

	struct TubExit {
		uint16_t                tub           = 0;
		uint16_t                source_module = 0;
		int8_t                  endpoint      = -1; // 0 left, 1 middle, 2 right; unavailable for non-physical removal.
		world::TubRemovalReason cause         = world::TubRemovalReason::EXPLICIT;
		uint64_t                tick          = 0;
		std::vector<uint8_t>    uid;
		std::vector<uint8_t>    data;
	};

	struct TubPosition {
		uint16_t        tub      = 0;
		uint16_t        module   = 0;
		world::Position position = world::Position::ENTRY;

		auto operator<=>(const TubPosition&) const = default;
	};

	struct ChangedField {
		uint16_t    module = 0;
		ModuleField field  = ModuleField::BIG_BELT_SPEED;

		auto operator<=>(const ChangedField&) const = default;
	};

	struct RpcRequest {
		uint64_t             id          = 0;
		uint64_t             tick        = 0;
		uint16_t             module      = 0;
		uint8_t              cmd         = 0;
		uint16_t             call_id     = 0;
		uint16_t             flags       = 0;
		bool                 confirmable = false;
		std::vector<uint8_t> payload;
	};

	struct RpcResponse {
		uint64_t             id         = 0;
		uint64_t             request_id = 0;
		uint64_t             tick       = 0;
		uint16_t             module     = 0;
		uint8_t              cmd        = 0;
		uint16_t             call_id    = 0;
		int16_t              status     = 0;
		std::vector<uint8_t> payload;
	};

	struct RpcEvent {
		uint64_t             id     = 0;
		uint64_t             tick   = 0;
		uint16_t             module = 0;
		uint8_t              kind   = 0;
		std::vector<uint8_t> payload;
	};

	class Observer;

	class Observation {
	public:
		[[nodiscard]] uint64_t tick() const noexcept { return tick_; }

		// Equal revisions certify equal LTL atom, domain, and module-step inputs, not equal time.
		[[nodiscard]] uint64_t revision() const noexcept { return revision_; }

		[[nodiscard]] std::chrono::milliseconds timestamp() const noexcept { return timestamp_; }

		[[nodiscard]] const std::map<uint16_t, ModuleSnapshot>& modules() const noexcept {
			static const std::map<uint16_t, ModuleSnapshot> empty;
			return modules_ ? *modules_ : empty;
		}

		[[nodiscard]] const std::map<uint16_t, TubSnapshot>& tubs() const noexcept { return tubs_; }

		[[nodiscard]] const std::set<uint16_t>& appeared() const noexcept { return appeared_; }

		[[nodiscard]] const std::set<uint16_t>& disappeared() const noexcept { return disappeared_; }

		[[nodiscard]] const std::set<TubPosition>& entered() const noexcept { return entered_; }

		[[nodiscard]] const std::set<TubPosition>& left() const noexcept { return left_; }

		[[nodiscard]] const std::set<ChangedField>& changed() const noexcept { return changed_; }

		// Whether a module's own clock granted this tick, so its application code advanced. A module
		// the runtime does not schedule is always considered to have advanced: the physical world and
		// any fixture behind it move on every tick.
		[[nodiscard]] bool stepped(uint16_t module) const noexcept {
			const auto found = module_stepped_.find(module);
			return found == module_stepped_.end() || found->second;
		}

		[[nodiscard]] const std::vector<RpcRequest>& rpc_requests() const noexcept { return rpc_requests_; }

		[[nodiscard]] const std::vector<RpcResponse>& rpc_responses() const noexcept { return rpc_responses_; }

		[[nodiscard]] const std::vector<RpcEvent>& rpc_events() const noexcept { return rpc_events_; }

		// Iterates a persistent typed domain without constructing a temporary collection.
		void visit_domain(const Value* expression, ValueType type, const std::function<void(uint64_t)>& visit) const;
		[[nodiscard]] const RpcRequest*  rpc_request(uint64_t id) const;
		[[nodiscard]] const RpcResponse* rpc_response_to(uint64_t request_id) const;
		[[nodiscard]] const RpcEvent*    rpc_event(uint64_t id) const;
		[[nodiscard]] const TubExit*     tub_exit(uint16_t id) const;
		[[nodiscard]] const TubSnapshot* initial_tub(uint16_t id) const;

		// Evaluates a typed ground atom or comparison. This is suitable for Monitor::observe().
		[[nodiscard]] bool evaluate(const Formula& atom) const;

		// Returns the observed left-hand value for a comparison. Non-comparison atoms have no
		// additional value beyond their Boolean result.
		[[nodiscard]] std::optional<std::string> actual_value(const Formula& atom) const;

	private:
		friend class Observer;

		uint64_t                                                  tick_     = 0;
		uint64_t                                                  revision_ = 0;
		std::chrono::milliseconds                                 timestamp_{0};
		std::shared_ptr<const std::map<uint16_t, ModuleSnapshot>> modules_;
		std::map<uint16_t, TubSnapshot>                           tubs_;
		std::set<uint16_t>                                        appeared_;
		std::set<uint16_t>                                        disappeared_;
		std::set<TubPosition>                                     entered_;
		std::set<TubPosition>                                     left_;
		std::set<ChangedField>                                    changed_;
		std::map<uint16_t, bool>                                  module_stepped_;
		std::set<uint16_t>                                        known_tubs_;
		std::vector<RpcRequest>                                   rpc_requests_;
		std::vector<RpcResponse>                                  rpc_responses_;
		std::vector<RpcEvent>                                     rpc_events_;
		const Observer*                                           observer_ = nullptr;
	};

	class Observer {
	public:
		// Captures the pre-application module baseline. Tubs are intentionally not a baseline: every
		// tub present in the first observation produces appeared and entered edges.
		Observer(const std::vector<module::ModuleId>& modules, const world::State& baseline);

		[[nodiscard]] Observation observe(uint64_t logical_tick, const world::State& world,
										  std::vector<RpcRequest>  requests  = {},
										  std::vector<RpcResponse> responses = {}, std::vector<RpcEvent> events = {},
										  std::map<uint16_t, bool> module_stepped = {});

		[[nodiscard]] const RpcRequest*  rpc_request(uint64_t id) const;
		[[nodiscard]] const RpcResponse* rpc_response_to(uint64_t request_id, uint64_t observation_tick) const;
		[[nodiscard]] const RpcEvent*    rpc_event(uint64_t id, uint64_t observation_tick) const;
		[[nodiscard]] const TubExit*     tub_exit(uint16_t id, uint64_t observation_tick) const;
		[[nodiscard]] const TubSnapshot* initial_tub(uint16_t id) const;

	private:
		uint64_t                                                  revision_               = 0;
		uint64_t                                                  previous_tick_          = 0;
		bool                                                      previous_pulses_        = false;
		bool                                                      time_sensitive_records_ = false;
		std::map<uint16_t, bool>                                  previous_stepped_;
		std::vector<module::ModuleId>                             module_ids_;
		std::shared_ptr<const std::map<uint16_t, ModuleSnapshot>> previous_modules_;
		std::map<uint16_t, TubSnapshot>                           previous_tubs_;
		std::set<uint16_t>                                        seen_tubs_;
		std::unordered_map<uint64_t, RpcRequest>                  rpc_request_registry_;
		std::unordered_map<uint64_t, RpcResponse>                 rpc_response_registry_;
		std::unordered_map<uint64_t, RpcEvent>                    rpc_event_registry_;
		std::unordered_map<uint16_t, TubExit>                     tub_exit_registry_;
		std::unordered_map<uint16_t, TubSnapshot>                 initial_tub_registry_;
	};

} // namespace ltl

#endif // LTL_OBSERVATION_H
