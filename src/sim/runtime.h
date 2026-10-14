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

#include "../engine/task_queue.h"
#include "../event/events.h"
#include "../firmware/inprocess_backend.h"
#include "../ltl/ast.h"
#include "../ltl/monitor.h"
#include "../ltl/observation.h"
#include "../module/types.h"
#include "../platform/platform.h"
#include "../platform/rpc_worker_pool.h"
#include "../trace/trace.h"
#include "../world/world.h"
#include "module_clock.h"
#include "pacing.h"

namespace sim {

	void                      pin_to_one_cpu() noexcept;
	inline constexpr uint64_t RPC_RX_DELAY_LOCAL_TICKS = 1;
	inline constexpr uint64_t RPC_TX_DELAY_LOCAL_TICKS = 1;

	struct PropertyResult {
		std::string                            name;
		std::string                            source;
		ltl::Position                          position;
		ltl::Verdict                           verdict = ltl::Verdict::PENDING;
		std::optional<uint64_t>                decisive_tick;
		std::optional<ltl::FailureExplanation> failure;
		uint64_t                               activations = 0;
		bool                                   has_trigger = false;
		std::string                            obligation_at_end;
	};

	// Every module owns a private coroutine image. Only the selected module's grant
	// advances its native time. The world and platform always use global time.
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

		struct ModuleRuntime {
			module::ModuleId                      id;
			ModuleClock                           clock;
			bool                                  tick_granted     = false;
			bool                                  tick_skipped     = false;
			bool                                  active           = true;
			bool                                  accepting_rpc    = true;
			bool                                  retire_pending   = false;
			bool                                  retire_after_ack = false;
			std::deque<rpc::Frame>                staged_outbound;
			std::deque<PendingRpcRequest>         pending_requests;
			std::deque<PendingRpcResponse>        pending_responses;
			platform::RpcWorkerPool               rpc_workers;
			std::map<uint64_t, PendingRpcRequest> rpc_work;
		};

		uint64_t                                    seed_;
		std::map<std::string, std::string>          environment_;
		std::vector<firmware::ImageBinding>         firmware_bindings_;
		std::vector<module::ModuleId>               active_modules_;
		std::vector<ModuleRuntime>                  modules_;
		std::vector<firmware::ExecutionGrant>       grants_;
		std::unique_ptr<firmware::InProcessBackend> backend_;
		uint64_t                                    generation_ = 0;
		rpc::FrameReader                            reader_;
		world::State                                world_;
		std::optional<platform::Manager>            platform_;
		engine::Queue                               queue_;
		uint64_t                                    global_tick_ = 0;
		std::array<uint64_t, MODULE_TABLE_SIZE>     module_ticks_{};
		int64_t                                     budget_us_ = 0;
		bool                                        running_   = false;

		double                                   effective_speed_      = 0.0;
		bool                                     effective_speed_seen_ = false;
		static constexpr std::size_t             PACING_WINDOW_FRAMES  = 10;
		std::array<double, PACING_WINDOW_FRAMES> pacing_actual_us_{};
		std::array<double, PACING_WINDOW_FRAMES> pacing_target_us_{};
		std::size_t                              pacing_window_index_ = 0;
		std::size_t                              pacing_window_count_ = 0;
		double                                   pacing_actual_sum_   = 0.0;
		double                                   pacing_target_sum_   = 0.0;
		double                                   pacing_window_speed_ = 0.0;
		bool                                     pacing_warning_      = false;
		trace::Writer                            trace_;

		std::optional<ltl::File>             properties_;
		std::vector<ltl::Monitor>            property_monitors_;
		std::vector<std::optional<uint64_t>> property_decisive_ticks_;
		using PropertyWakeups = std::multimap<uint64_t, std::size_t>;

		struct PropertySchedule {
			std::vector<uint32_t>                    reads;
			std::optional<PropertyWakeups::iterator> wakeup;
			uint64_t                                 revision = 0;
			bool                                     dirty    = true;
		};

		std::vector<PropertySchedule>                     property_schedules_;
		PropertyWakeups                                   property_wakeups_;
		std::size_t                                       property_bitset_words_ = 0;
		std::vector<uint64_t>                             property_subscribers_;
		std::vector<uint64_t>                             property_ready_;
		std::vector<uint64_t>                             property_changed_;
		std::vector<uint32_t>                             property_scratch_reads_;
		uint64_t                                          next_property_revision_ = 0;
		std::optional<ltl::Observer>                      property_observer_;
		std::optional<ltl::Observation>                   current_observation_;
		uint64_t                                          scenario_duration_tick_ = 0;
		bool                                              halt_on_failure_        = true;
		std::string                                       property_filename_;
		uint64_t                                          next_rpc_observation_id_ = 1;
		uint64_t                                          next_rpc_work_id_        = 1;
		std::map<uint64_t, std::vector<ltl::RpcRequest>>  pending_rpc_requests_;
		std::map<uint64_t, std::vector<ltl::RpcResponse>> pending_rpc_responses_;
		std::map<uint64_t, std::vector<ltl::RpcEvent>>    pending_rpc_events_;
		std::map<uint8_t, std::string>                    last_world_;

		void trace_observation(const ltl::Observation& observation);
		void wake_property(std::size_t index, bool input_changed);
		void wake_properties(const ltl::Observation& observation);
		void subscribe_property(std::size_t index, const std::vector<uint32_t>& reads);
		void intake(ModuleRuntime& instance, uint64_t global_tick, uint64_t local_tick,
					const firmware::FrameBatchView& batch);
		void release_local_rpc(ModuleRuntime& instance, uint64_t global_tick, uint64_t local_tick);
		void start_rpc_work(ModuleRuntime& instance, uint64_t work_id, uint64_t global_tick, uint64_t local_tick);
		void quiesce_module_for_stop(ModuleRuntime& instance, uint64_t local_tick, std::optional<rpc::Frame> response);
		void retire_module(module::ModuleId module, uint64_t tick);
		void schedule_tick_tasks();
		void step();
		void report_pacing(std::chrono::microseconds elapsed, double speed, uint64_t ticks_advanced);

	public:
		explicit Runtime(uint64_t seed, std::vector<firmware::ImageBinding> firmware = {},
						 std::map<std::string, std::string> environment = {});
		~Runtime();
		Runtime(const Runtime&)            = delete;
		Runtime& operator=(const Runtime&) = delete;
		void     start(std::optional<uint64_t> last_tick = std::nullopt);
		void     trace_to(const std::string& path, const std::vector<std::string>& inputs);
		void     advance(std::chrono::microseconds elapsed, double speed, std::chrono::microseconds deadline);
		void     load(const events::Schedule& schedule);
		void     load_properties(ltl::File properties, uint64_t duration_tick, const std::string& filename = "<ltl>");
		void     stop();

		[[nodiscard]] uint64_t tick() const noexcept { return global_tick_; }

		[[nodiscard]] uint64_t module_tick(module::ModuleId module) const noexcept {
			return module_ticks_[module.raw()];
		}

		[[nodiscard]] bool failed() const noexcept;

		[[nodiscard]] bool halted() const noexcept { return halt_on_failure_ && failed(); }

		void set_halt_on_failure(bool halt) noexcept { halt_on_failure_ = halt; }

		[[nodiscard]] std::size_t properties_total() const noexcept { return property_monitors_.size(); }

		[[nodiscard]] std::size_t                 properties_pending() const noexcept;
		[[nodiscard]] std::size_t                 properties_failed() const noexcept;
		[[nodiscard]] std::size_t                 properties_unresolved() const noexcept;
		[[nodiscard]] std::vector<PropertyResult> property_results() const;

		[[nodiscard]] world::State& world() noexcept { return world_; }

		[[nodiscard]] const world::State& world() const noexcept { return world_; }

		[[nodiscard]] platform::Manager& platform() { return *platform_; }

		[[nodiscard]] const platform::Manager& platform() const { return *platform_; }
	};
} // namespace sim
#endif
