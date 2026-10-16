#ifndef LTL_OBSERVATION_H
#define LTL_OBSERVATION_H

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
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
		uint16_t previous_module = 0;

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

	// An input channel is one independently changing fact that atom evaluation can read: a module
	// output field, one field of one tub, the records one module produced this tick, and so on.
	// A monitor that records the channels it read can skip a step whose channels did not change.
	namespace channel {
		enum class TubField : uint8_t {
			PRESENT,
			MODULE,
			BELT,
			POSITION_UM,
			POSITION,
			PREVIOUS_MODULE,
			UID,
			DATA,
			APPEARED,
			DISAPPEARED,
			EDGES,
			EXIT,
			INITIAL,
		};

		enum class RecordList : uint8_t { REQUESTS, RESPONSES, EVENTS };

		constexpr uint32_t ENTITIES      = 256;
		constexpr uint32_t MODULE_FIELDS = 0;
		constexpr uint32_t MODULE_EDGES  = MODULE_FIELDS + ENTITIES * 16;
		constexpr uint32_t TUB_FIELDS    = MODULE_EDGES + ENTITIES * 16;
		constexpr uint32_t RECORD_LISTS  = TUB_FIELDS + ENTITIES * 16;
		constexpr uint32_t STEPPED       = RECORD_LISTS + ENTITIES * 4;
		constexpr uint32_t TUB_DOMAIN    = STEPPED + ENTITIES;
		// Any read that has no exact channel. It changes on every observation.
		constexpr uint32_t VOLATILE         = TUB_DOMAIN + 1;
		constexpr uint32_t RESPONSE_BUCKETS = 4096;
		constexpr uint32_t RESPONSES        = VOLATILE + 1;
		constexpr uint32_t COUNT            = RESPONSES + RESPONSE_BUCKETS;

		constexpr uint32_t module_field(uint16_t module, ModuleField field) {
			return module < ENTITIES ? MODULE_FIELDS + module * 16u + static_cast<uint32_t>(field) : VOLATILE;
		}

		constexpr uint32_t module_edge(uint16_t module, ModuleField field) {
			return module < ENTITIES ? MODULE_EDGES + module * 16u + static_cast<uint32_t>(field) : VOLATILE;
		}

		constexpr uint32_t tub(uint16_t id, TubField field) {
			return id < ENTITIES ? TUB_FIELDS + id * 16u + static_cast<uint32_t>(field) : VOLATILE;
		}

		constexpr uint32_t records(uint16_t module, RecordList list) {
			return module < ENTITIES ? RECORD_LISTS + module * 4u + static_cast<uint32_t>(list) : VOLATILE;
		}

		constexpr uint32_t stepped(uint16_t module) {
			return module < ENTITIES ? STEPPED + module : VOLATILE;
		}

		constexpr uint32_t response(uint64_t request_id) {
			return RESPONSES + static_cast<uint32_t>(request_id % RESPONSE_BUCKETS);
		}
	} // namespace channel

	// Whether each module's own clock granted a tick. Modules without an entry count as stepped.
	class ModuleSteps {
	public:
		ModuleSteps() = default;

		ModuleSteps(std::initializer_list<std::pair<uint16_t, bool>> entries) {
			for (const auto& [module, stepped] : entries) emplace(module, stepped);
		}

		// Like std::map::emplace(): the first entry for a module is kept.
		void emplace(uint16_t module, bool stepped);

		// The entry of a module, if any.
		[[nodiscard]] std::optional<bool> find(uint16_t module) const noexcept {
			if (module >= channel::ENTITIES || states_[module] == NONE) return std::nullopt;
			return states_[module] == STEPPED;
		}

		bool operator==(const ModuleSteps&) const = default;

	private:
		// Plain bytes, so that comparing two tables is one memcmp().
		static constexpr uint8_t NONE = 0, SKIPPED = 1, STEPPED = 2;

		std::array<uint8_t, channel::ENTITIES> states_{};
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

		[[nodiscard]] const std::map<uint16_t, TubSnapshot>& tubs() const noexcept {
			static const std::map<uint16_t, TubSnapshot> empty;
			return tubs_ ? *tubs_ : empty;
		}

		[[nodiscard]] const std::set<uint16_t>& appeared() const noexcept { return appeared_; }

		[[nodiscard]] const std::set<uint16_t>& disappeared() const noexcept { return disappeared_; }

		[[nodiscard]] const std::set<TubPosition>& entered() const noexcept { return entered_; }

		[[nodiscard]] const std::set<TubPosition>& left() const noexcept { return left_; }

		[[nodiscard]] const std::set<ChangedField>& changed() const noexcept { return changed_; }

		// Whether a module's own clock granted this tick, so its application code advanced. A module
		// the runtime does not schedule is always considered to have advanced: the physical world and
		// any fixture behind it move on every tick.
		[[nodiscard]] bool stepped(uint16_t module) const noexcept {
			note(channel::stepped(module));
			return module_stepped_.find(module).value_or(true);
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

		// Evaluates an atom of an unground formula with its quantifier bindings supplied.
		[[nodiscard]] bool evaluate(const Formula&                                       atom,
									const std::vector<std::pair<std::string, uint64_t>>& bindings) const;

		// Returns the observed left-hand value for a comparison. Non-comparison atoms have no
		// additional value beyond their Boolean result.
		[[nodiscard]] std::optional<std::string> actual_value(const Formula& atom) const;

		// While set, every channel that evaluation reads is appended to reads, possibly repeated.
		void track(std::vector<uint32_t>* reads) const noexcept { reads_ = reads; }

		void note(uint32_t channel) const {
			if (reads_) reads_->push_back(channel);
		}

		// Whether any of the channels changed after the given tick. Out-of-order observations and
		// future-dated records make visibility depend on time, so they report every channel changed.
		[[nodiscard]] bool changed_since(const std::vector<uint32_t>& channels, uint64_t tick) const;

		// Removes repeated channels in place, keeping first occurrences, in linear time.
		void deduplicate(std::vector<uint32_t>& channels) const;

		// The channels whose value changed on this observation, each once.
		[[nodiscard]] const std::vector<uint32_t>& changed_channels() const;

		// Whether visibility depends on time, so that every channel must count as changed.
		[[nodiscard]] bool volatile_inputs() const;

	private:
		friend class Observer;

		mutable std::vector<uint32_t>* reads_ = nullptr;

		uint64_t                                                  tick_     = 0;
		uint64_t                                                  revision_ = 0;
		std::chrono::milliseconds                                 timestamp_{0};
		std::shared_ptr<const std::map<uint16_t, ModuleSnapshot>> modules_;
		std::shared_ptr<const std::map<uint16_t, TubSnapshot>>    tubs_;
		std::set<uint16_t>                                        appeared_;
		std::set<uint16_t>                                        disappeared_;
		std::set<TubPosition>                                     entered_;
		std::set<TubPosition>                                     left_;
		std::set<ChangedField>                                    changed_;
		ModuleSteps                                               module_stepped_;
		std::shared_ptr<const std::set<uint16_t>>                 known_tubs_;
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
										  const ModuleSteps& module_stepped = {});

		[[nodiscard]] const RpcRequest*  rpc_request(uint64_t id) const;
		[[nodiscard]] const RpcResponse* rpc_response_to(uint64_t request_id, uint64_t observation_tick) const;
		[[nodiscard]] const RpcEvent*    rpc_event(uint64_t id, uint64_t observation_tick) const;
		[[nodiscard]] const TubExit*     tub_exit(uint16_t id, uint64_t observation_tick) const;
		[[nodiscard]] const TubSnapshot* initial_tub(uint16_t id) const;

		[[nodiscard]] bool changed_since(const std::vector<uint32_t>& channels, uint64_t tick) const;

		void deduplicate(std::vector<uint32_t>& channels) const;

		[[nodiscard]] const std::vector<uint32_t>& changed_channels() const noexcept { return changed_now_; }

		[[nodiscard]] bool volatile_inputs() const noexcept { return time_sensitive_records_; }

	private:
		void mark(uint32_t channel, uint64_t tick) {
			if (changed_tick_[channel] == tick) return;
			changed_tick_[channel] = tick;
			changed_now_.push_back(channel);
		}

		void mark_tub(uint16_t id, uint64_t tick);
		void derive_channel_changes(const Observation& observation);

		// The last observation tick on which each channel's value changed.
		std::vector<uint64_t>  changed_tick_ = std::vector<uint64_t>(channel::COUNT, 0);
		std::vector<uint32_t>  changed_now_;
		std::vector<uint32_t>  previous_record_channels_;
		std::set<ChangedField> previous_changed_;
		std::set<uint16_t>     previous_appeared_;
		std::set<uint16_t>     previous_disappeared_;
		std::set<uint16_t>     previous_edge_tubs_;
		std::size_t            seen_count_ = 0;
		// Scratch for deduplicate(): the generation in which each channel was last kept.
		mutable std::vector<uint32_t> channel_stamps_         = std::vector<uint32_t>(channel::COUNT, 0);
		mutable uint32_t              channel_generation_     = 0;
		uint64_t                      revision_               = 0;
		uint64_t                      previous_tick_          = 0;
		bool                          previous_pulses_        = false;
		bool                          time_sensitive_records_ = false;
		ModuleSteps                   previous_stepped_;
		std::vector<module::ModuleId> module_ids_;
		std::shared_ptr<const std::map<uint16_t, ModuleSnapshot>> previous_modules_;
		// Unchanged snapshots are shared between consecutive observations.
		std::shared_ptr<const std::map<uint16_t, TubSnapshot>> previous_tubs_;
		std::shared_ptr<const std::set<uint16_t>> seen_tubs_ = std::make_shared<const std::set<uint16_t>>();
		std::unordered_map<uint64_t, RpcRequest>  rpc_request_registry_;
		std::unordered_map<uint64_t, RpcResponse> rpc_response_registry_;
		std::unordered_map<uint64_t, RpcEvent>    rpc_event_registry_;
		std::unordered_map<uint16_t, TubExit>     tub_exit_registry_;
		std::unordered_map<uint16_t, TubSnapshot> initial_tub_registry_;
	};

} // namespace ltl

#endif // LTL_OBSERVATION_H
