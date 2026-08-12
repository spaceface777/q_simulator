#ifndef PLATFORM_PLATFORM_H
#define PLATFORM_PLATFORM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../childsync/mailbox.h"
#include "../defs.h"
#include "../module/module.h"
#include "../module/types.h"
#include "../net/network.h"
#include "../rpc/frame.h"

namespace world {
	class State;
}

namespace platform {

	// Application lifecycle, mirroring the states the physical platform tracks per application.
	enum class Lifecycle : uint8_t {
		STOPPED,
		ACTIVATING,
		RUNNING,
		PAUSED,
	};

	// Negated errno values returned to applications.
	inline constexpr int16_t STATUS_OK        = 0;
	inline constexpr int16_t STATUS_PERM      = -1;  // -EPERM
	inline constexpr int16_t STATUS_NO_ENTRY  = -2;  // -ENOENT
	inline constexpr int16_t STATUS_TRY_AGAIN = -11; // -EAGAIN
	inline constexpr int16_t STATUS_INVALID   = -22; // -EINVAL
	inline constexpr int16_t STATUS_RANGE     = -34; // -ERANGE
	inline constexpr int16_t STATUS_NO_SYS    = -38; // -ENOSYS

	// Frames the platform may deliver to one application per tick. The child's mailbox is one page
	// holding four messages, so the byte budget can bite before the frame count does.
	inline constexpr std::size_t MAX_OUTBOUND_PER_TICK = childsync::MAX_FRAMES_PER_TICK;

	// The window rate limits are measured over. Capacities are quoted per simulated second.
	inline constexpr uint64_t RATE_LIMIT_WINDOW_TICKS = 1000;

	// How long a paused application is withheld its ticks.
	inline constexpr uint64_t PAUSE_DURATION_TICKS = 1000;

	// Heartbeat supervision, in ticks of simulated time.
	inline constexpr uint64_t ACTIVATION_DEADLINE_TICKS = 10000;
	inline constexpr uint64_t HEARTBEAT_WARNING_TICKS   = 500;
	inline constexpr uint64_t HEARTBEAT_FAILURE_TICKS   = 2000;

	// The rate-limit buckets a command can be charged to.
	enum class Bucket : uint8_t {
		GLOBAL,
		SCHEDULE,
		NETWORK,
		LOGGING,
		RFID,
		BELT,
		SERVO,
		PLATFORM_MANAGEMENT,
		HEARTBEAT,
		EVENTS,
		LASER,
		LED,
		CV_SENSOR,
		COUNT,
	};

	// Something the platform noticed about an application's behaviour.
	struct Diagnostic {
		uint64_t         tick = 0;
		module::ModuleId module;
		Bucket           bucket = Bucket::GLOBAL;
		std::string      kind;
		int              count = 0;
		int              limit = 0;
	};

	// The bucket's name, for diagnostics and traces.
	[[nodiscard]] std::string to_string(Bucket bucket);

	// How many requests per simulated second the bucket allows.
	[[nodiscard]] int capacity_of(Bucket bucket);

	// Per-application state the platform owns. None of it lives in the child.
	struct Runtime {
		bool                     present           = false;
		uint32_t                 subscriptions     = 0;
		bool                     heartbeat_seen    = false;
		Lifecycle                lifecycle         = Lifecycle::ACTIVATING;
		uint64_t                 paused_until_tick = 0;
		std::vector<std::string> logs;
		std::vector<std::string> panics;

		// One sliding window of request ticks per bucket, and the tick each bucket last warned
		// at, so that a sustained overload warns once rather than every request.
		std::array<std::deque<uint64_t>, static_cast<std::size_t>(Bucket::COUNT)> bucket_samples;
		std::array<uint64_t, static_cast<std::size_t>(Bucket::COUNT)>             bucket_warned_tick{};
		std::array<bool, static_cast<std::size_t>(Bucket::COUNT)>                 bucket_has_warned{};

		uint64_t    last_heartbeat_tick = 0;
		bool        heartbeat_warned    = false;
		std::string stop_reason;

		// Edge detection state. Events report transitions, so the platform remembers what the
		// application was last told rather than resending the current level.
		bool                 last_left_clear  = false;
		bool                 last_right_clear = false;
		std::vector<uint8_t> last_rfid_uid;

		std::deque<rpc::Frame> outbound;
	};

	class Manager {
	private:
		// Keyed by module id rather than indexed by it, to avoid a fixed 256-entry table.
		std::map<uint8_t, Runtime> runtimes_;

		[[nodiscard]] Runtime*        find(module::ModuleId id);
		[[nodiscard]] const Runtime*  find(module::ModuleId id) const;
		std::string                   network_map_;
		std::vector<module::ModuleId> modules_;
		std::vector<uint8_t>          flight_schedule_;
		net::Model                    network_;

		// Logs and panic reports share this delivery model; each still draws its own outcome.
		net::Model log_network_;

		world::State* world_ = nullptr;

		std::vector<Diagnostic> diagnostics_;

		[[nodiscard]] rpc::Frame dispatch(module::ModuleId id, const rpc::Frame& request, uint64_t tick);

		[[nodiscard]] bool charge(module::ModuleId id, uint8_t cmd, uint64_t tick);

		void publish(module::ModuleId id, uint32_t event, std::vector<uint8_t> payload);

		[[nodiscard]] bool enqueue_packet(uint64_t tick, module::ModuleId sender, const rpc::Frame& request,
										  bool broadcast);

	public:
		explicit Manager(world::State& world);

		// Exposed for tests; parse_settings drives this normally.
		void add_module(module::ModuleId id);

		void rebuild_topology();

		// Returns the response to send, or nothing when the request was non-confirmable.
		[[nodiscard]] std::optional<rpc::Frame> handle_request(module::ModuleId id, const rpc::Frame& request,
															   uint64_t tick = 0);

		void prepare_tick(uint64_t tick);

		// Throws std::runtime_error if an application missed its deadline or was stopped.
		void check_supervision(uint64_t tick);

		[[nodiscard]] std::vector<Diagnostic> take_diagnostics();

		// Turns world state and network traffic into application-visible events, landing them in
		// the per-module backlog rather than sending immediately.
		void advance(uint64_t tick);

		void enqueue(module::ModuleId id, rpc::Frame frame);

		// Takes up to MAX_OUTBOUND_PER_TICK frames from a module's backlog, stopping early at the
		// first frame that would not fit in byte_budget. What is left keeps its place in the queue.
		[[nodiscard]] std::vector<rpc::Frame> take_outbound(module::ModuleId id, std::size_t byte_budget);

		[[nodiscard]] std::size_t backlog(module::ModuleId id) const;

		// The events the module is subscribed to, as a mask of rpc_event_t.
		[[nodiscard]] uint32_t subscriptions(module::ModuleId id) const;

		[[nodiscard]] Lifecycle lifecycle(module::ModuleId id) const;

		[[nodiscard]] bool is_paused(module::ModuleId id, uint64_t tick) const;

		[[nodiscard]] bool is_activated(module::ModuleId id) const;

		[[nodiscard]] const std::vector<std::string>& logs(module::ModuleId id) const;

		[[nodiscard]] const std::vector<std::string>& panics(module::ModuleId id) const;

		[[nodiscard]] const std::vector<uint8_t>& flight_schedule() const noexcept { return flight_schedule_; }

		[[nodiscard]] const std::string& network_map() const noexcept { return network_map_; }
	};

} // namespace platform

#endif // PLATFORM_PLATFORM_H
