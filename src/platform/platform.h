#ifndef PLATFORM_PLATFORM_H
#define PLATFORM_PLATFORM_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../defs.h"
#include "../module/module.h"
#include "../module/types.h"
#include "../net/network.h"
#include "../rpc/frame.h"
#include <quercus/sim_transport.h>

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
	inline constexpr int16_t STATUS_NO_SYS    = -88; // -ENOSYS

	// A grant carries at most four frames within its bounded byte buffer.
	inline constexpr std::size_t MAX_OUTBOUND_PER_TICK = QSIM_MAX_GRANT_FRAMES;

	// Physical rate windows use Core0 uptime, so they keep aging while Core1 is paused.
	inline constexpr uint64_t RATE_LIMIT_WINDOW_TICKS = 1000;

	// Forced pauses and heartbeat supervision belong to the external platform and use global time.
	// A pause cannot use module-local time because the paused module is deliberately granted no ticks.
	inline constexpr uint64_t PAUSE_DURATION_TICKS = 1000;

	// Heartbeat supervision, in global physical ticks.
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

	// A request crosses two clock domains: physical/network effects use the shared global tick,
	// while per-application service delays and rate limits use only ticks that module executed.
	struct ModuleTime {
		uint64_t global_tick = 0;
		uint64_t local_tick  = 0;
	};

	// The result of charging one frame at the receive boundary. It follows a decoded request to
	// command admission so the global bucket is checked without being incremented again.
	struct IngressAdmission {
		bool allowed        = false;
		bool stop_triggered = false;
	};

	// The bucket's name, for diagnostics and traces.
	[[nodiscard]] std::string to_string(Bucket bucket);

	// How many requests per simulated second the bucket allows.
	[[nodiscard]] int capacity_of(Bucket bucket);

	// The per-category mutex a command uses on Core0.
	[[nodiscard]] Bucket request_bucket(uint8_t cmd);

	// Per-module lifecycle and RPC state owned by the platform.
	struct Runtime {
		bool                     present                  = false;
		uint32_t                 subscriptions            = 0;
		bool                     heartbeat_seen           = false;
		Lifecycle                lifecycle                = Lifecycle::ACTIVATING;
		uint64_t                 paused_until_global_tick = 0;
		bool                     pause_resume_pending     = false;
		bool                     belt_restore_valid       = false;
		module::BeltSpeed        restore_big_belt_speed;
		module::BeltSpeed        restore_small_belt_speed;
		std::vector<std::string> logs;
		std::vector<std::string> panics;

		// One sliding window of Core0/global request ticks per bucket, and the global tick last warned
		// at, so that a sustained overload warns once rather than every request.
		std::array<std::deque<uint64_t>, static_cast<std::size_t>(Bucket::COUNT)> bucket_samples;
		std::array<uint64_t, static_cast<std::size_t>(Bucket::COUNT)>             bucket_warned_tick{};
		std::array<bool, static_cast<std::size_t>(Bucket::COUNT)>                 bucket_has_warned{};

		uint64_t    last_heartbeat_global_tick = 0;
		bool        heartbeat_warned           = false;
		bool        heartbeat_failed           = false;
		bool        rate_stopped               = false;
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
		struct PendingHardwareEvent {
			uint64_t         due_tick = 0;
			uint64_t         ordinal  = 0;
			module::ModuleId module;
			rpc::Frame       frame;
		};

		// Keyed by module id rather than indexed by it, to avoid a fixed 256-entry table.
		std::map<uint8_t, Runtime> runtimes_;
		std::set<uint8_t>          known_modules_;

		[[nodiscard]] Runtime*        find(module::ModuleId id);
		[[nodiscard]] const Runtime*  find(module::ModuleId id) const;
		std::string                   network_map_;
		std::vector<module::ModuleId> modules_;
		std::vector<uint8_t>          flight_schedule_;
		net::Model                    network_;

		// Logs and panic reports share this delivery model; each still draws its own outcome.
		net::Model log_network_;

		uint64_t                                         master_seed_                 = 0;
		uint64_t                                         laser_event_extra_delay_ms_  = 0;
		double                                           rfid_event_delay_mean_ms_    = 0.0;
		double                                           rfid_event_delay_sd_ms_      = 0.0;
		uint64_t                                         next_hardware_event_ordinal_ = 0;
		std::map<std::pair<uint8_t, uint32_t>, uint64_t> hardware_event_draw_ordinals_;
		std::vector<PendingHardwareEvent>                pending_hardware_events_;

		world::State* world_ = nullptr;

		std::vector<Diagnostic> diagnostics_;

		[[nodiscard]] rpc::Frame dispatch(module::ModuleId id, const rpc::Frame& request, uint64_t global_tick);

		struct ChargeResult {
			bool allowed        = false;
			bool stop_triggered = false;
		};

		[[nodiscard]] ChargeResult charge_bucket(module::ModuleId id, Bucket bucket, ModuleTime time);

		[[nodiscard]] ChargeResult charge_category(module::ModuleId id, uint8_t cmd, ModuleTime time);

		void publish(module::ModuleId id, uint32_t event, std::vector<uint8_t> payload);

		void schedule_hardware_event(uint64_t tick, module::ModuleId id, uint32_t event, std::vector<uint8_t> payload,
									 uint64_t delay_ms);

		[[nodiscard]] uint64_t sample_rfid_event_delay_ms(module::ModuleId id);

		void deliver_hardware_events(uint64_t tick);

		[[nodiscard]] bool enqueue_packet(uint64_t tick, module::ModuleId sender, const rpc::Frame& request,
										  bool broadcast);

		void pause_application(module::ModuleId id, uint64_t tick);

		void stop_application(uint64_t tick, module::ModuleId id, std::string reason, Bucket cause,
							  bool heartbeat_failure);

	public:
		explicit Manager(world::State& world);

		// Exposed for tests; parse_settings drives this normally.
		void add_module(module::ModuleId id);

		void rebuild_topology();

		// Charges every frame as soon as it reaches the platform receive boundary, before decoding.
		[[nodiscard]] IngressAdmission account_ingress(module::ModuleId id, ModuleTime time = {});

		// Returns the response to send, or nothing when the request was non-confirmable.
		[[nodiscard]] std::optional<rpc::Frame> handle_request(module::ModuleId id, const rpc::Frame& request,
															   ModuleTime                      time    = {},
															   std::optional<IngressAdmission> ingress = std::nullopt);

		void prepare_tick(uint64_t tick);

		// Stops applications that missed their heartbeat deadline and returns heartbeat-failed modules.
		// Rate-stopped modules remain observable but are retired by the runtime's safe-stop transaction.
		std::vector<module::ModuleId> check_supervision(uint64_t tick);

		[[nodiscard]] std::vector<Diagnostic> take_diagnostics();

		// Current samples in a bucket's sliding window; useful for platform observability and tests.
		[[nodiscard]] std::size_t rate_sample_count(module::ModuleId id, Bucket bucket) const;

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

		[[nodiscard]] const std::string& stop_reason(module::ModuleId id) const;

		[[nodiscard]] bool is_paused(module::ModuleId id, uint64_t tick) const;

		[[nodiscard]] bool is_activated(module::ModuleId id) const;

		[[nodiscard]] const std::vector<std::string>& logs(module::ModuleId id) const;

		[[nodiscard]] const std::vector<std::string>& panics(module::ModuleId id) const;

		[[nodiscard]] const std::vector<uint8_t>& flight_schedule() const noexcept { return flight_schedule_; }

		[[nodiscard]] const std::string& network_map() const noexcept { return network_map_; }
	};

} // namespace platform

#endif // PLATFORM_PLATFORM_H
