#ifndef SIM_RUNTIME_H
#define SIM_RUNTIME_H

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../childproc/launcher.h"
#include "../childsync/barrier.h"
#include "../engine/task_queue.h"
#include "../event/events.h"
#include "../ltl/ast.h"
#include "../ltl/monitor.h"
#include "../ltl/observation.h"
#include "../module/types.h"
#include "../platform/platform.h"
#include "../platform/rpc_worker_pool.h"
#include "../trace/trace.h"
#include "../world/world.h"
#include "pacing.h"

namespace sim {

	// Short lockstep bursts run faster without cross-core wakeups and cache-line bouncing. Child
	// processes and their Zephyr threads inherit this thread's affinity when they are launched.
	void pin_to_one_cpu() noexcept;

	// Strict mode barriers every tick; batched mode grants a fixed-size window at each barrier.

	inline constexpr std::chrono::microseconds TICK_DURATION{1000};
	inline constexpr uint32_t                  MAX_TICK_BATCH     = static_cast<uint32_t>(childsync::MAX_BATCH_TICKS);
	inline constexpr uint32_t                  MAX_RUNAHEAD_TICKS = QUERCUS_RUNAHEAD_MAX_TICKS;
	inline constexpr uint32_t                  DEFAULT_RUNAHEAD_TICKS = 100;

	// These delays count ticks executed by the requesting module, not global simulation ticks.
	inline constexpr uint64_t RPC_RX_DELAY_LOCAL_TICKS = 1;
	inline constexpr uint64_t RPC_TX_DELAY_LOCAL_TICKS = 1;

	// How long the parent blocks in one futex wait before re-checking how far behind the tick is
	// and whether a child has died. A healthy tick is released by the last child and never waits
	// this long, so the cost only lands on a run that is already stalling.
	inline constexpr std::chrono::microseconds BARRIER_POLL{5000};

	inline constexpr std::chrono::milliseconds CONNECT_TIMEOUT{10000};

	// The platform calls a child's first heartbeat "activation".
	inline constexpr std::chrono::milliseconds ACTIVATION_TIMEOUT{20000};

	struct PropertyResult {
		std::string                            name;
		std::string                            source;
		ltl::Position                          position;
		ltl::Verdict                           verdict = ltl::Verdict::PENDING;
		std::optional<uint64_t>                decisive_tick;
		std::optional<ltl::FailureExplanation> failure;

		// How often the property imposed an obligation, and whether that number means anything for
		// this property. A conditional property that never triggered passed without testing anything.
		uint64_t    activations = 0;
		bool        has_trigger = false;
		std::string obligation_at_end;
	};

	class Runtime {
	private:
		struct PendingRpcRequest {
			uint64_t                   due_local_tick = 0;
			uint64_t                   work_id        = 0;
			rpc::Frame                 frame;
			std::string                label;
			uint64_t                   observation_id = 0;
			platform::IngressAdmission ingress;
		};

		struct PendingRpcResponse {
			uint64_t   due_local_tick = 0;
			rpc::Frame frame;
		};

		struct DeferredGrant {
			bool     granted  = false;
			uint32_t delta_us = 0;
		};

		struct Child {
			module::ModuleId                       id;
			childproc::Handle                      process;
			std::optional<childsync::ChildSegment> segment;
			uint64_t                               start_tick       = 0;
			int64_t                                clock_rate_ppm   = 1'000'000;
			int64_t                                clock_remainder  = 0;
			uint32_t                               tick_skip_ppm    = 0;
			uint64_t                               tick_skip_rng    = 0;
			uint16_t                               grant_mask       = 0;
			uint16_t                               skipped_mask     = 0;
			bool                                   tick_granted     = false;
			bool                                   active           = true;
			bool                                   accepting_rpc    = true;
			bool                                   retire_pending   = false;
			bool                                   retire_after_ack = false;
			std::deque<rpc::Frame>                 staged_outbound;
			std::deque<PendingRpcRequest>          pending_requests;
			std::deque<PendingRpcResponse>         pending_responses;
			platform::RpcWorkerPool                rpc_workers;
			std::map<uint64_t, PendingRpcRequest>  rpc_work;
			std::deque<DeferredGrant>              deferred_grants;
			uint32_t                               wake_seq        = 0;
			uint8_t                                scheduler_index = 0;
			bool                                   selected        = false;
		};

		struct FutureGrant {
			std::array<bool, MAX_RUNAHEAD_TICKS>     granted{};
			std::array<bool, MAX_RUNAHEAD_TICKS>     skipped{};
			std::array<uint32_t, MAX_RUNAHEAD_TICKS> delta_us{};
		};

		enum class BoundaryLimit : uint8_t {
			CHILD_WAKE,
			ENGINE_DELIVERY,
			HARD_LOOKAHEAD_CAP,
			CONSERVATIVE_FALLBACK,
			COUNT,
		};

		struct BoundaryProbe {
			uint32_t                                probed_ticks = 0;
			BoundaryLimit                           limit        = BoundaryLimit::CONSERVATIVE_FALLBACK;
			std::array<uint32_t, MODULE_TABLE_SIZE> child_horizon{};
			std::array<bool, MODULE_TABLE_SIZE>     child_proven{};
			std::array<bool, MODULE_TABLE_SIZE>     child_wake{};
			std::array<bool, MODULE_TABLE_SIZE>     engine_delivery{};
			std::array<bool, MODULE_TABLE_SIZE>     active{};

			BoundaryProbe() { child_horizon.fill(UINT32_MAX); }
		};

		struct SynchronizationStats {
			uint64_t                                                             logical_ticks          = 0;
			uint64_t                                                             boundaries             = 0;
			uint64_t                                                             current_child_wakeups  = 0;
			uint64_t                                                             required_child_wakeups = 0;
			std::array<uint64_t, MAX_RUNAHEAD_TICKS + 1>                         ticks_per_boundary{};
			std::array<uint64_t, MODULE_TABLE_SIZE + 1>                          required_wake_set_sizes{};
			std::array<uint64_t, static_cast<std::size_t>(BoundaryLimit::COUNT)> limits{};
			std::array<uint64_t, MODULE_TABLE_SIZE>                              current_module_wakes{};
			std::array<uint64_t, MODULE_TABLE_SIZE>                              required_module_wakes{};
		};

		uint64_t                      seed_;
		std::chrono::milliseconds     connect_timeout_;
		std::chrono::milliseconds     activation_timeout_;
		uint32_t                      tick_batch_;
		uint32_t                      runahead_cap_;
		bool                          selective_sync_;
		std::vector<module::ModuleId> active_modules_;
		std::vector<Child>            children_;

		// The barrier words every child shares. Created before the first launch.
		std::optional<childsync::SharedControl> control_;

		// Advances once per barrier: once per strict tick or once per batched window.
		uint32_t generation_ = 0;

		// Reused across mailbox messages so decoding a frame does not allocate every tick.
		rpc::FrameReader reader_;

		// Owned here, not global, so a run can be created more than once.
		world::State                     world_;
		std::optional<platform::Manager> platform_;
		engine::Queue                    queue_;
		// These are the simulator's only authoritative clocks. Shared-memory values and trace
		// fields are outbound snapshots; neither is ever read back to reconstruct time.
		uint64_t                                global_tick_ = 0;
		std::array<uint64_t, MODULE_TABLE_SIZE> module_ticks_{};
		int64_t                                 budget_us_           = 0;
		bool                                    running_             = false;
		uint64_t                                runahead_windows_    = 0;
		uint64_t                                runahead_ticks_      = 0;
		uint64_t                                runahead_fallbacks_  = 0;
		bool                                    sync_stats_enabled_  = false;
		bool                                    sync_stats_modules_  = false;
		bool                                    sync_stats_reported_ = false;
		SynchronizationStats                    sync_stats_;
		std::optional<BoundaryProbe>            pending_strict_probe_;
		uint32_t                                selective_ticks_since_sync_ = 0;

		// Smoothed simulated-over-real time: one slow frame is normal, a sustained shortfall is not.
		double effective_speed_      = 0.0;
		bool   effective_speed_seen_ = false;

		// Logging uses a finite aggregate window so an isolated slow tick or frame stays quiet.
		static constexpr std::size_t             PACING_WINDOW_FRAMES = 10;
		std::array<double, PACING_WINDOW_FRAMES> pacing_actual_us_{};
		std::array<double, PACING_WINDOW_FRAMES> pacing_target_us_{};

		std::size_t   pacing_window_index_ = 0;
		std::size_t   pacing_window_count_ = 0;
		double        pacing_actual_sum_   = 0.0;
		double        pacing_target_sum_   = 0.0;
		double        pacing_window_speed_ = 0.0;
		bool          pacing_warning_      = false;
		trace::Writer trace_;

		// The parsed file owns the formulas referenced by the monitors and therefore precedes them.
		std::optional<ltl::File>                          properties_;
		std::vector<ltl::Monitor>                         property_monitors_;
		std::vector<std::optional<uint64_t>>              property_decisive_ticks_;
		std::optional<ltl::Observer>                      property_observer_;
		std::optional<ltl::Observation>                   current_observation_;
		uint64_t                                          scenario_duration_tick_ = 0;
		bool     halt_on_failure_ = true;
		std::string                                       property_filename_;
		uint64_t                                          next_rpc_observation_id_ = 1;
		uint64_t                                          next_rpc_work_id_        = 1;
		std::map<uint64_t, std::vector<ltl::RpcRequest>>  pending_rpc_requests_;
		std::map<uint64_t, std::vector<ltl::RpcResponse>> pending_rpc_responses_;
		std::map<uint64_t, std::vector<ltl::RpcEvent>>    pending_rpc_events_;

		// The state the last trace entry described, so a tick records only what changed.
		std::map<uint8_t, std::string> last_world_;

		void trace_observation(const ltl::Observation& observation);

		// Drops undecodable application frames after ingress accounting. Corrupt mailbox metadata,
		// which cannot be indexed safely, remains fatal.
		void drain(Child& child, uint64_t source_global_tick, uint64_t source_local_tick, uint32_t batch_index = 0);

		void release_local_rpc(Child& child, uint64_t global_tick, uint64_t local_tick);

		void start_rpc_work(Child& child, uint64_t work_id, uint64_t global_tick, uint64_t local_tick);

		void quiesce_module_for_stop(Child& child, uint64_t local_tick, std::optional<rpc::Frame> final_response);

		void retire_module(module::ModuleId id, uint64_t tick);

		void grant_tick(uint32_t batch_ticks);

		[[nodiscard]] std::vector<FutureGrant> plan_future_grants(uint32_t ticks) const;

		[[nodiscard]] uint32_t child_safe_horizon(const std::vector<FutureGrant>& grants, uint32_t max_ticks,
												  BoundaryProbe* probe = nullptr) const;

		[[nodiscard]] uint32_t parent_safe_horizon(uint32_t max_ticks, BoundaryProbe* probe = nullptr) const;

		void grant_runahead(const std::vector<FutureGrant>& grants, uint32_t ticks);

		// Blocks until every child has answered the barrier. A child that never answers is waited
		// for, not killed, because skipping its tick would change the run.
		void collect_acknowledgements();

		void schedule_tick_tasks(uint32_t batch_index, uint32_t batch_ticks);

		uint32_t step(uint32_t batch_ticks);

		uint32_t step_exact(uint32_t max_ticks);

		uint32_t step_selective(uint32_t batch_ticks);

		void grant_selective(const std::vector<FutureGrant>& grants, uint32_t batch_ticks,
							 const std::vector<bool>& selected);

		[[nodiscard]] bool selective_child_due(const Child& child, const FutureGrant& grant,
											   uint32_t batch_ticks) const;

		void capture_active_modules(BoundaryProbe& probe) const;

		void mark_required_child_wakes(BoundaryProbe& probe, uint32_t boundary_ticks) const;

		void record_synchronization(uint32_t logical_ticks, BoundaryProbe probe);

		void report_synchronization_stats();

		void report_pacing(std::chrono::microseconds elapsed, double speed, uint64_t ticks_advanced);

	public:
		// Governs every ordering decision, and is handed to every application at launch.
		explicit Runtime(uint64_t seed, std::chrono::milliseconds connect_timeout = CONNECT_TIMEOUT,
						 std::chrono::milliseconds activation_timeout = ACTIVATION_TIMEOUT, uint32_t tick_batch = 1,
						 uint32_t runahead_cap = 0, bool selective_sync = false);

		~Runtime();

		Runtime(const Runtime&)            = delete;
		Runtime& operator=(const Runtime&) = delete;

		// Throws if a child fails to connect or activate in time. When last_tick is present, returns
		// after observing that inclusive logical tick even if activation is still in progress.
		void start(std::optional<uint64_t> last_tick = std::nullopt);

		// path is where to write the trace; empty disables it.
		void trace_to(const std::string& path, const std::vector<std::string>& inputs);

		// Throws if a child dies.
		void advance(std::chrono::microseconds elapsed, double speed, std::chrono::microseconds deadline);

		// Anything due at tick zero describes the starting world, so it is applied before the run.
		void load(const events::Schedule& schedule);

		// Compiles property monitors before applications launch. duration_tick is the inclusive end
		// of the finite scenario trace. The parsed file is retained because monitors reference its AST.
		void load_properties(ltl::File properties, uint64_t duration_tick, const std::string& filename = "<ltl>");

		void stop();

		[[nodiscard]] uint64_t tick() const noexcept { return global_tick_; }

		[[nodiscard]] uint64_t module_tick(module::ModuleId id) const noexcept { return module_ticks_[id.raw()]; }

		[[nodiscard]] uint64_t runahead_windows() const noexcept { return runahead_windows_; }

		[[nodiscard]] uint64_t runahead_ticks() const noexcept { return runahead_ticks_; }

		[[nodiscard]] uint64_t runahead_fallbacks() const noexcept { return runahead_fallbacks_; }

		// Once true, the run grants no further ticks, so the world stays in the state that failed.
		[[nodiscard]] bool failed() const noexcept;

		// Whether the run stops granting ticks. By default a failure halts the run; with halting
		// disabled every property is still evaluated to the end of the scenario.
		[[nodiscard]] bool halted() const noexcept { return halt_on_failure_ && failed(); }
		void               set_halt_on_failure(bool halt) noexcept { halt_on_failure_ = halt; }

		[[nodiscard]] std::size_t properties_pending() const noexcept;

		// Properties the trace ended too early to decide. These are not refutations, but they are
		// not verifications either, so a run that leaves any is not a clean run.
		[[nodiscard]] std::size_t properties_unresolved() const noexcept;

		[[nodiscard]] std::size_t properties_failed() const noexcept;

		[[nodiscard]] std::size_t properties_total() const noexcept { return property_monitors_.size(); }

		[[nodiscard]] std::vector<PropertyResult> property_results() const;

		[[nodiscard]] platform::Manager& platform() noexcept { return platform_.value(); }

		[[nodiscard]] world::State& world() noexcept { return world_; }
	};

} // namespace sim

#endif // SIM_RUNTIME_H
