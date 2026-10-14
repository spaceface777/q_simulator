#include "runtime.h"

#include <algorithm>
#include <bit>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#ifdef __linux__
#include <sched.h>
#endif
#include <sstream>
#include <stdexcept>
#include <unistd.h>

#include "../firmware/inprocess_backend.h"
#include "../ltl/checker.h"
#include "../module/module.h"
#include "../seed/seed.h"
#include "../settings.h"
#include "../trace/trace.h"
#include "../world/world.h"

namespace sim {

	namespace {

		// How much of the previous speed measurement survives into the next.
		constexpr double SPEED_SMOOTHING = 0.9;

		// Below this shortfall, treat it as measurement noise rather than being behind.
		constexpr double BEHIND_THRESHOLD = 0.9;

		// A log message requires a sustained shortfall; the GUI can still show smaller fluctuations.
		constexpr double WARNING_THRESHOLD = 0.95;

		constexpr uint64_t TRACE_FLUSH_TICKS = 100;

		using Clock = std::chrono::steady_clock;

		const char* verdict_name(ltl::Verdict verdict) {
			switch (verdict) {
			case ltl::Verdict::FALSE:      return "false";
			case ltl::Verdict::TRUE:       return "true";
			case ltl::Verdict::PENDING:    return "pending";
			case ltl::Verdict::UNRESOLVED: return "unresolved";
			}
			return "pending";
		}

		std::string payload_hex(const std::vector<uint8_t>& payload) {
			static constexpr char digits[] = "0123456789abcdef";
			std::string           encoded;
			encoded.reserve(payload.size() * 2);
			for (const uint8_t byte : payload) {
				encoded += digits[byte >> 4];
				encoded += digits[byte & 0x0f];
			}
			return encoded;
		}

		double uniform_open(seed::SplitMix64& stream) {
			return (static_cast<double>(stream.next() >> 11) + 1.0) / 9007199254740993.0;
		}

		double standard_normal(seed::SplitMix64& stream) {
			const double u1 = uniform_open(stream);
			const double u2 = uniform_open(stream);
			return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
		}

	} // namespace

	void pin_to_one_cpu() noexcept {
#ifdef __linux__
		const int cpu = ::sched_getcpu();
		if (cpu < 0 || cpu >= CPU_SETSIZE) return;
		cpu_set_t selected;
		CPU_ZERO(&selected);
		CPU_SET(cpu, &selected);
		if (::sched_setaffinity(0, sizeof(selected), &selected) != 0)
			std::cerr << "Could not pin simulation: " << std::strerror(errno) << std::endl;
#endif
	}

	Runtime::Runtime(uint64_t seed, std::vector<firmware::ImageBinding> firmware,
					 std::map<std::string, std::string> environment)
		: seed_(seed), environment_(std::move(environment)), firmware_bindings_(std::move(firmware)),
		  active_modules_(settings::get_active_modules()), queue_(seed) {
		if (active_modules_.empty()) throw std::runtime_error("No active modules in layout.toml");
		for (const module::ModuleId id : active_modules_) world_.add_module(id);
		property_observer_.emplace(active_modules_, world_);
	}

	Runtime::~Runtime() {
		stop();
	}

	void Runtime::start(std::optional<uint64_t> last_tick) {
		if (running_ || backend_) throw std::logic_error("The runtime is already started");
		if (global_tick_ != 0) throw std::logic_error("Create a new runtime for another run");
		for (const module::ModuleId id : active_modules_) {
			ModuleRuntime instance;
			instance.id = id;
			seed::SplitMix64 timing(seed::derive(seed_, "module-clock/" + std::to_string(id.raw())));
			const double     start_ms =
				settings::get_setting<double>("module_start_offset_mean_ms") +
				settings::get_setting<double>("module_start_offset_sd_ms") * standard_normal(timing);
			const uint64_t start_tick = start_ms <= 0.0 ? 0u : static_cast<uint64_t>(std::llround(start_ms));
			const double   drift = settings::get_setting<double>("module_clock_drift_sd_ppm") * standard_normal(timing);
			const int64_t  rate_ppm = std::max<int64_t>(1, 1'000'000 + static_cast<int64_t>(std::llround(drift)));
			const double   skip_ppm_sample =
				settings::get_setting<double>("module_tick_skip_mean_ppm") +
				settings::get_setting<double>("module_tick_skip_sd_ppm") * standard_normal(timing);
			const uint32_t skip_ppm =
				static_cast<uint32_t>(std::clamp(std::llround(skip_ppm_sample), 0LL, 1'000'000LL));
			instance.clock = ModuleClock(start_tick, rate_ppm, skip_ppm,
										 seed::derive(seed_, "module-clock-skips/" + std::to_string(id.raw())));
			std::cout << "Starting module " << static_cast<unsigned>(id.raw());
			std::cout << " at tick " << instance.clock.start_tick() << " with clock " << instance.clock.rate_ppm()
					  << " ppm" << std::endl;
			std::cout << "  tick skip chance " << instance.clock.skip_ppm() << " ppm" << std::endl;
			if (trace_.enabled()) {
				trace_.write(trace::Entry("module_clock")
								 .field("module", static_cast<uint64_t>(id.raw()))
								 .field("start_tick", instance.clock.start_tick())
								 .field("rate_ppm", instance.clock.rate_ppm())
								 .field("skip_ppm", static_cast<uint64_t>(instance.clock.skip_ppm())));
			}
			modules_.push_back(std::move(instance));
		}
		grants_.resize(modules_.size());
		try {
			auto bindings = firmware_bindings_;
			if (bindings.empty())
				for (const auto id : active_modules_) bindings.push_back({id, settings::get_module_firmware(id)});
			for (const auto id : active_modules_) {
				if (std::ranges::count(bindings, id, &firmware::ImageBinding::module) != 1)
					throw std::invalid_argument("Module " + std::to_string(id.raw()) + " requires one firmware image");
				const auto binding = std::ranges::find(bindings, id, &firmware::ImageBinding::module);
				if (binding->path.empty())
					throw std::invalid_argument("Module " + std::to_string(id.raw()) + " has no firmware");
			}
			for (const auto& binding : bindings)
				if (std::ranges::find(active_modules_, binding.module) == active_modules_.end())
					throw std::invalid_argument("Firmware configured for inactive module " +
												std::to_string(binding.module.raw()));
			backend_ = std::make_unique<firmware::InProcessBackend>(bindings, seed_, environment_);
		} catch (...) {
			stop();
			throw;
		}

		running_ = true;
		platform_.emplace(world_);

		std::cout << "Waiting for applications to activate" << std::endl;

		// An application activates by sending its first heartbeat, which requires being ticked first.
		while (true) {
			const bool all_active =
				std::all_of(modules_.begin(), modules_.end(), [this](const ModuleRuntime& instance) {
					return platform_->is_activated(instance.id) ||
						   platform_->lifecycle(instance.id) == platform::Lifecycle::STOPPED;
				});
			if (all_active) {
				break;
			}
			step();
			if (halted() || (!property_monitors_.empty() && properties_pending() == 0)) break;
			if (last_tick && global_tick_ > *last_tick) break;
		}
	}

	void Runtime::trace_to(const std::string& path, const std::vector<std::string>& inputs) {
		trace_ = trace::Writer(path);
		if (!trace_.enabled()) {
			return;
		}

		// Digests record which inputs produced this trace, so only like traces are compared.
		trace::Entry entry("run_start");
		entry.field("seed", seed_)
			.field("tick_us", static_cast<uint64_t>(TICK_DURATION.count()))
			.field("ltlf_version", std::string("0.3"))
			.field("scenario_duration_tick", scenario_duration_tick_);
		for (const std::string& input : inputs) {
			entry.field(input, trace::digest_file(input));
		}
		entry.field("environment", environment_);
		entry.field("modules", static_cast<uint64_t>(active_modules_.size()));
		entry.field("tick_batch_requested", uint64_t{1}).field("tick_batch_effective", uint64_t{1});
		trace_.write(entry);
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			const uint64_t     until_tick = declaration.scope ? declaration.scope->until_tick : scenario_duration_tick_;
			const std::string& source     = declaration.span.begin.source_name().empty()
												? property_filename_
												: declaration.span.begin.source_name();
			trace_.write(trace::Entry("property_activation")
							 .field("name", declaration.name)
							 .field("source", source)
							 .field("line", static_cast<uint64_t>(declaration.span.begin.line))
							 .field("column", static_cast<uint64_t>(declaration.span.begin.column))
							 .field("from_tick", from_tick)
							 .field("until_tick", until_tick)
							 .field("formula", property_monitors_[index].formula()));
		}
	}

	void Runtime::trace_observation(const ltl::Observation& observation) {
		if (!trace_.enabled()) {
			return;
		}

		for (const auto& [id, tub] : observation.tubs()) {
			// One line per tub, only when it moved.
			const std::string current  = std::to_string(tub.module) + "/" + std::to_string(static_cast<int>(tub.belt)) +
										 "/" + std::to_string(tub.position_um);
			std::string&      previous = last_world_[id];
			if (previous == current) {
				continue;
			}
			previous = current;

			trace_.write(trace::Entry("tub")
							 .field("tick", observation.tick())
							 .field("id", static_cast<uint64_t>(tub.id))
							 .field("module", static_cast<uint64_t>(tub.module))
							 .field("belt", static_cast<uint64_t>(static_cast<int>(tub.belt)))
							 .field("position_um", static_cast<int64_t>(tub.position_um)));
		}
		for (const uint16_t id : observation.disappeared()) {
			const world::TubRemovalReason reason =
				world_.removal_reason(static_cast<uint8_t>(id)).value_or(world::TubRemovalReason::EXPLICIT);
			trace_.write(trace::Entry("tub_removed")
							 .field("tick", observation.tick())
							 .field("id", static_cast<uint64_t>(id))
							 .field("reason", world::to_string(reason)));
		}

		for (const ltl::RpcRequest& request : observation.rpc_requests()) {
			trace_.write(trace::Entry("rpc_request")
							 .field("tick", request.tick)
							 .field("id", request.id)
							 .field("module", static_cast<uint64_t>(request.module))
							 .field("cmd", static_cast<uint64_t>(request.cmd))
							 .field("call_id", static_cast<uint64_t>(request.call_id))
							 .field("flags", static_cast<uint64_t>(request.flags))
							 .field("confirmable", request.confirmable)
							 .field("payload", payload_hex(request.payload)));
		}
		for (const ltl::RpcResponse& response : observation.rpc_responses()) {
			trace_.write(trace::Entry("rpc_response")
							 .field("tick", response.tick)
							 .field("id", response.id)
							 .field("request_id", response.request_id)
							 .field("module", static_cast<uint64_t>(response.module))
							 .field("cmd", static_cast<uint64_t>(response.cmd))
							 .field("call_id", static_cast<uint64_t>(response.call_id))
							 .field("status", static_cast<int64_t>(response.status))
							 .field("payload", payload_hex(response.payload)));
		}
		for (const ltl::RpcEvent& event : observation.rpc_events()) {
			trace_.write(trace::Entry("rpc_event")
							 .field("tick", event.tick)
							 .field("id", event.id)
							 .field("module", static_cast<uint64_t>(event.module))
							 .field("kind", static_cast<uint64_t>(event.kind))
							 .field("payload", payload_hex(event.payload)));
		}
	}

	void Runtime::load(const events::Schedule& schedule) {
		if (running_ || global_tick_ != 0) {
			throw std::logic_error("Scenario events must be loaded before applications launch");
		}

		for (const events::TubArrival& tub : schedule.tubs) {
			const uint8_t          id       = tub.id;
			const module::ModuleId module   = tub.module;
			const world::Position  position = tub.position;
			const RfidUid          uid      = tub.uid;
			const RfidPayload      payload  = tub.payload;

			if (tub.tick == 0) {
				world_.place(id, module, position, uid, payload);
				continue;
			}

			queue_.schedule(tub.tick, engine::Priority::SCENARIO_EVENTS, "tub/" + std::to_string(id),
							[this, id, module, position, uid, payload](engine::TaskContext&) {
								world_.place(id, module, position, uid, payload);
							});
		}

		for (const events::TubArrival& tub : schedule.tubs) {
			if (!tub.removed_tick) {
				continue;
			}
			const uint8_t id = tub.id;
			queue_.schedule(
				*tub.removed_tick, engine::Priority::SCENARIO_EVENTS, "tub/" + std::to_string(id) + "/removed",
				[this, id](engine::TaskContext&) { world_.remove(id, world::TubRemovalReason::SCHEDULED); });
		}
	}

	void Runtime::load_properties(ltl::File properties, uint64_t duration_tick, const std::string& filename) {
		if (running_ || global_tick_ != 0)
			throw std::logic_error("LTL properties must be loaded before applications launch");
		if (properties_) throw std::logic_error("LTL properties have already been loaded");

		std::vector<uint16_t> module_ids;
		module_ids.reserve(active_modules_.size());
		for (const module::ModuleId id : active_modules_) module_ids.push_back(id.raw());
		ltl::type_check(properties, ltl::TypeEnvironment{std::move(module_ids), duration_tick}, filename);

		properties_.emplace(std::move(properties));
		property_monitors_.reserve(properties_->properties.size());
		property_decisive_ticks_.reserve(properties_->properties.size());
		for (const ltl::PropertyDeclaration& declaration : properties_->properties) {
			const uint64_t from_tick = declaration.scope ? declaration.scope->from_tick : 0;
			property_monitors_.emplace_back(declaration.name, *declaration.formula, from_tick);
			property_decisive_ticks_.emplace_back();
			property_schedules_.emplace_back();
			property_schedules_.back().wakeup = property_wakeups_.emplace(from_tick, property_monitors_.size() - 1);
		}
		property_bitset_words_ = (property_monitors_.size() + 63) / 64;
		property_subscribers_.resize(ltl::channel::COUNT * property_bitset_words_);
		property_ready_.resize(property_bitset_words_);
		property_changed_.resize(property_bitset_words_);
		scenario_duration_tick_ = duration_tick;
		property_filename_      = filename;
	}

	void Runtime::wake_property(std::size_t index, bool input_changed) {
		PropertySchedule& schedule = property_schedules_[index];
		// An input wake need not discard its timer: a self-loop often retains the same deadline.
		// Reuse it until progression changes that deadline, instead of allocating a new timer on
		// every input pulse. A timer wake consumes the entry that is due now.
		if (!input_changed && schedule.wakeup) {
			property_wakeups_.erase(*schedule.wakeup);
			schedule.wakeup.reset();
		}
		schedule.dirty |= input_changed;
		property_ready_[index / 64] |= uint64_t{1} << (index % 64);
	}

	void Runtime::wake_properties(const ltl::Observation& observation) {
		if (observation.volatile_inputs()) {
			// Out-of-order or future-dated observations need time-dependent visibility. Use the
			// conservative path: every pending monitor observes every tick.
			for (std::size_t index = 0; index < property_monitors_.size(); index++)
				if (!property_decisive_ticks_[index]) wake_property(index, true);
		} else {
			std::ranges::fill(property_changed_, 0);
			for (const uint32_t channel : observation.changed_channels()) {
				const std::size_t start = channel * property_bitset_words_;
				for (std::size_t word = 0; word < property_bitset_words_; word++)
					property_changed_[word] |= property_subscribers_[start + word];
			}
			for (std::size_t word = 0; word < property_bitset_words_; word++) {
				uint64_t changed = property_changed_[word];
				while (changed) {
					const std::size_t index = word * 64 + std::countr_zero(changed);
					changed &= changed - 1;
					wake_property(index, true);
				}
			}
		}
		// One live timer per sleeping monitor, removed if input wakes it before its boundary.
		while (!property_wakeups_.empty() && property_wakeups_.begin()->first <= observation.tick())
			wake_property(property_wakeups_.begin()->second, false);
	}

	void Runtime::subscribe_property(std::size_t index, const std::vector<uint32_t>& reads) {
		PropertySchedule& schedule = property_schedules_[index];
		if (schedule.reads == reads) return;
		const std::size_t word = index / 64;
		const uint64_t    bit  = uint64_t{1} << (index % 64);
		for (const uint32_t channel : schedule.reads)
			property_subscribers_[channel * property_bitset_words_ + word] &= ~bit;
		for (const uint32_t channel : reads) property_subscribers_[channel * property_bitset_words_ + word] |= bit;
	}

	bool Runtime::failed() const noexcept {
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			if (from_tick <= scenario_duration_tick_ && global_tick_ > from_tick &&
				property_monitors_[index].verdict() == ltl::Verdict::FALSE)
				return true;
		}
		return false;
	}

	std::size_t Runtime::properties_pending() const noexcept {
		std::size_t pending = 0;
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			if (from_tick > scenario_duration_tick_ || global_tick_ <= from_tick ||
				property_monitors_[index].verdict() == ltl::Verdict::PENDING)
				pending++;
		}
		return pending;
	}

	std::size_t Runtime::properties_unresolved() const noexcept {
		std::size_t unresolved = 0;
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			if (from_tick <= scenario_duration_tick_ && global_tick_ > from_tick &&
				property_monitors_[index].verdict() == ltl::Verdict::UNRESOLVED)
				unresolved++;
		}
		return unresolved;
	}

	std::size_t Runtime::properties_failed() const noexcept {
		std::size_t failed = 0;
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			if (from_tick <= scenario_duration_tick_ && global_tick_ > from_tick &&
				property_monitors_[index].verdict() == ltl::Verdict::FALSE)
				failed++;
		}
		return failed;
	}

	std::vector<PropertyResult> Runtime::property_results() const {
		std::vector<PropertyResult> results;
		results.reserve(property_monitors_.size());
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			const ltl::Verdict              verdict = (from_tick > scenario_duration_tick_ || global_tick_ <= from_tick)
														  ? ltl::Verdict::PENDING
														  : property_monitors_[index].verdict();
			const ltl::Monitor&             monitor = property_monitors_[index];
			// Pending self-loops may not have been stepped since they fell asleep. Include those
			// repetitions when reporting an early-halted run without mutating its monitor state.
			const uint64_t     last_tick = global_tick_ ? std::min(global_tick_ - 1, scenario_duration_tick_) : 0;
			const uint64_t     repeats   = global_tick_ && monitor.repeats_until() && last_tick >= monitor.next_tick()
											   ? last_tick - monitor.next_tick() + 1
											   : 0;
			const std::string& source    = declaration.span.begin.source_name().empty()
											   ? property_filename_
											   : declaration.span.begin.source_name();
			results.push_back(PropertyResult{declaration.name, source, declaration.span.begin, verdict,
											 property_decisive_ticks_[index], monitor.failure(),
											 monitor.activations_after_repeats(repeats), monitor.has_trigger(),
											 monitor.obligation_at_end()});
		}
		return results;
	}

	void Runtime::advance(std::chrono::microseconds elapsed, double speed, std::chrono::microseconds deadline) {
		if (!running_ || speed <= 0.0 || halted()) {
			return;
		}

		budget_us_ += static_cast<int64_t>(static_cast<double>(elapsed.count()) * speed);

		int64_t granted = budget_us_ / TICK_DURATION.count();
		budget_us_ -= granted * TICK_DURATION.count();
		if (budget_us_ < 0) {
			budget_us_ = 0;
		}

		const Clock::time_point started     = Clock::now();
		const Clock::time_point limit       = started + deadline;
		const uint64_t          tick_before = global_tick_;

		for (int64_t index = 0; index < granted; ++index) {
			step();

			// A decisive property result ends the run inside the tick that produced it, state intact.
			if (halted() || (!property_monitors_.empty() && properties_pending() == 0)) {
				budget_us_ = 0;
				report_pacing(elapsed, speed, global_tick_ - tick_before);
				return;
			}

			if (Clock::now() > limit) {
				// Behind schedule: drop the rest of the budget instead of granting it as a burst later.
				budget_us_ = 0;
				report_pacing(elapsed, speed, global_tick_ - tick_before);
				return;
			}
		}

		report_pacing(elapsed, speed, global_tick_ - tick_before);
	}

	void Runtime::report_pacing(std::chrono::microseconds elapsed, double speed, uint64_t ticks_advanced) {
		if (elapsed.count() <= 0) {
			return;
		}

		// Simulated time over real time is what the speed multiplier asks for, so measure against that.
		const double simulated_us = static_cast<double>(ticks_advanced * static_cast<uint64_t>(TICK_DURATION.count()));
		const double sample       = simulated_us / static_cast<double>(elapsed.count());
		const double target_us    = static_cast<double>(elapsed.count()) * speed;

		if (!effective_speed_seen_) {
			effective_speed_      = sample;
			effective_speed_seen_ = true;
		} else {
			effective_speed_ = (effective_speed_ * SPEED_SMOOTHING) + (sample * (1.0 - SPEED_SMOOTHING));
		}

		Pacing report;
		report.configured = speed;
		report.effective  = effective_speed_;
		report.behind     = effective_speed_ < (speed * BEHIND_THRESHOLD);
		publish_pacing(report);

		// A speed change starts a new observation window; samples from the previous target say
		// nothing about whether the new target is sustainable.
		if (speed != pacing_window_speed_) {
			pacing_window_index_ = 0;
			pacing_window_count_ = 0;
			pacing_actual_sum_   = 0.0;
			pacing_target_sum_   = 0.0;
			pacing_window_speed_ = speed;
			pacing_warning_      = false;
		}

		if (pacing_window_count_ == PACING_WINDOW_FRAMES) {
			pacing_actual_sum_ -= pacing_actual_us_[pacing_window_index_];
			pacing_target_sum_ -= pacing_target_us_[pacing_window_index_];
		} else {
			pacing_window_count_++;
		}

		pacing_actual_us_[pacing_window_index_] = simulated_us;
		pacing_target_us_[pacing_window_index_] = target_us;
		pacing_actual_sum_ += simulated_us;
		pacing_target_sum_ += target_us;
		pacing_window_index_ = (pacing_window_index_ + 1) % PACING_WINDOW_FRAMES;

		const bool sustained_shortfall = pacing_window_count_ == PACING_WINDOW_FRAMES &&
										 pacing_actual_sum_ < (pacing_target_sum_ * WARNING_THRESHOLD);
		if (sustained_shortfall && !pacing_warning_) {
			std::cerr << "Tick " << global_tick_
					  << ": simulation has remained at least 5% below the selected speed over the "
					  << "last " << PACING_WINDOW_FRAMES << " frames; this workload may not be sustainable"
					  << std::endl;
		}
		pacing_warning_ = sustained_shortfall;
	}

	void Runtime::step() {
		for (std::size_t index = 0; index < modules_.size(); index++) {
			ModuleRuntime&            instance = modules_[index];
			firmware::ExecutionGrant& grant    = grants_[index];
			grant.reset_metadata(instance.id, instance.active);
			instance.tick_skipped = false;
			if (!instance.active) continue;

			const bool paused =
				global_tick_ >= instance.clock.start_tick() && platform_->is_paused(instance.id, global_tick_);
			const ModuleClockTick clock_tick = instance.clock.advance(global_tick_, true, paused);
			grant.granted                    = clock_tick.granted;
			grant.delta_us                   = clock_tick.delta_us;
			grant.global_tick                = global_tick_;
			grant.local_tick                 = module_ticks_[instance.id.raw()];
			instance.tick_skipped            = clock_tick.skipped;
			if (!clock_tick.granted) continue;

			while (!instance.staged_outbound.empty()) {
				const rpc::Frame&          frame = instance.staged_outbound.front();
				const std::vector<uint8_t> bytes = rpc::encode(frame);
				if (qsim_frame_batch_append(&grant.inbound, &grant.inbound_used, grant.inbound_count, bytes.data(),
											static_cast<uint32_t>(bytes.size())) != QSIM_BATCH_OK)
					break;

				const uint64_t completed_work_id = frame.handler_work_id;
				const bool     stop_after_ack    = frame.stop_after_delivery;
				instance.staged_outbound.pop_front();
				grant.inbound_count++;
				if (stop_after_ack) instance.retire_after_ack = true;
				if (completed_work_id != 0) {
					for (const uint64_t work_id : instance.rpc_workers.complete(completed_work_id)) {
						start_rpc_work(instance, work_id, global_tick_, module_ticks_[instance.id.raw()]);
					}
				}
			}
		}

		// Complete preparation for every module before any firmware output can be observed.
		platform_->prepare_tick(global_tick_);
		generation_++;
		backend_->execute_epoch(grants_);

		for (ModuleRuntime& instance : modules_) {
			if (instance.active && instance.retire_after_ack) retire_module(instance.id, global_tick_);
		}
		for (std::size_t index = 0; index < modules_.size(); index++) {
			ModuleRuntime&                  instance = modules_[index];
			const firmware::ExecutionGrant& grant    = grants_[index];
			instance.tick_granted                    = instance.active && grant.granted;
			if (instance.tick_skipped && trace_.enabled()) {
				trace_.write(trace::Entry("module_tick_skipped")
								 .field("tick", global_tick_)
								 .field("module", static_cast<uint64_t>(instance.id.raw()))
								 .field("local_tick", module_ticks_[instance.id.raw()]));
			}
			if (!instance.tick_granted) continue;

			const uint64_t local_tick = module_ticks_[instance.id.raw()];
			intake(instance, global_tick_, local_tick, grant.outbound);
			release_local_rpc(instance, global_tick_, local_tick);
			module_ticks_[instance.id.raw()]++;
		}

		schedule_tick_tasks();
		queue_.run_tick(global_tick_);
		// Flush after all task generations of each 100-tick interval. stop() flushes the remainder.
		if (global_tick_ % TRACE_FLUSH_TICKS == TRACE_FLUSH_TICKS - 1) trace_.flush();
		global_tick_++;
	}

	void Runtime::retire_module(module::ModuleId id, uint64_t tick) {
		auto found = std::ranges::find_if(modules_, [id](const ModuleRuntime& instance) { return instance.id == id; });
		if (found == modules_.end() || !found->active) {
			return;
		}

		ModuleRuntime& instance   = *found;
		instance.active           = false;
		instance.accepting_rpc    = false;
		instance.tick_granted     = false;
		instance.tick_skipped     = false;
		instance.retire_pending   = false;
		instance.retire_after_ack = false;
		instance.staged_outbound.clear();
		instance.pending_requests.clear();
		instance.pending_responses.clear();
		instance.rpc_workers = platform::RpcWorkerPool{};
		instance.rpc_work.clear();
		backend_->retire(id);

		if (trace_.enabled()) {
			trace_.write(trace::Entry("module_stopped")
							 .field("tick", tick)
							 .field("module", static_cast<uint64_t>(id.raw()))
							 .field("reason", platform_->stop_reason(id)));
		}
	}

	void Runtime::intake(ModuleRuntime& instance, uint64_t source_global_tick, uint64_t source_local_tick,
						 const firmware::FrameBatchView& batch) {
		if (!batch.valid()) {
			throw std::runtime_error("Module " + std::to_string(instance.id.raw()) +
									 " supplied an invalid frame batch");
		}

		for (std::size_t index = 0; index < batch.size(); index++) {
			const firmware::FrameBatchView::Frame bytes = batch[index];
			const platform::IngressAdmission      ingress =
				platform_->account_ingress(instance.id, platform::ModuleTime{source_global_tick, source_local_tick});
			rpc::Frame frame;
			reader_.clear();
			reader_.append(bytes.data(), bytes.size());
			if (reader_.next(frame) != rpc::DecodeStatus::OK) {
				if (trace_.enabled()) {
					trace_.write(trace::Entry("rpc_ingress_drop")
									 .field("tick", source_global_tick)
									 .field("module", static_cast<uint64_t>(instance.id.raw()))
									 .field("reason", "malformed_frame")
									 .field("length", static_cast<uint64_t>(bytes.size())));
				}
				if (ingress.stop_triggered) quiesce_module_for_stop(instance, source_local_tick, std::nullopt);
				continue;
			}

			if (frame.type != MT_REQ) {
				if (trace_.enabled()) {
					trace_.write(trace::Entry("rpc_ingress_drop")
									 .field("tick", source_global_tick)
									 .field("module", static_cast<uint64_t>(instance.id.raw()))
									 .field("reason", "unexpected_frame_type")
									 .field("frame_type", static_cast<uint64_t>(frame.type)));
				}
				continue;
			}

			uint64_t observation_id = 0;
			if (trace_.enabled() || !property_monitors_.empty()) {
				observation_id = next_rpc_observation_id_++;
				pending_rpc_requests_[source_global_tick].push_back(
					ltl::RpcRequest{observation_id, source_global_tick, instance.id.raw(), frame.cmd, frame.call_id,
									frame.flags, (frame.flags & RPC_FLAG_NON_CON) == 0, frame.payload});
			}
			if (ingress.stop_triggered) {
				std::optional<rpc::Frame> terminal;
				if ((frame.flags & RPC_FLAG_NON_CON) == 0) {
					rpc::Frame response;
					response.type                   = MT_RESP;
					response.cmd                    = frame.cmd;
					response.call_id                = frame.call_id;
					response.status                 = platform::STATUS_TRY_AGAIN;
					response.observation_request_id = observation_id;
					response.stop_after_delivery    = true;
					terminal                        = std::move(response);
				}
				quiesce_module_for_stop(instance, source_local_tick, std::move(terminal));
				return;
			}

			// Requests become tasks so their order is decided by the seed, not by mailbox order.
			// The module, batch index, call ID, and command identify each request within a tick.
			// Task ordering uses the seeded queue; these decimal labels are not numeric sort keys.
			const module::ModuleId id = instance.id;
			char                   prefix[32];
			std::snprintf(prefix, sizeof(prefix), "rpc/%02u/%u/", id.raw(), static_cast<unsigned>(index));
			const std::string label = prefix + std::to_string(frame.call_id) + "/" + std::to_string(frame.cmd);

			instance.pending_requests.push_back(PendingRpcRequest{source_local_tick + RPC_RX_DELAY_LOCAL_TICKS,
																  next_rpc_work_id_++, std::move(frame),
																  std::move(label), observation_id, ingress});
		}
	}

	void Runtime::release_local_rpc(ModuleRuntime& instance, uint64_t global_tick, uint64_t local_tick) {
		while (!instance.pending_responses.empty() && instance.pending_responses.front().due_local_tick <= local_tick) {
			platform_->enqueue(instance.id, std::move(instance.pending_responses.front().frame));
			instance.pending_responses.pop_front();
		}

		while (!instance.pending_requests.empty() && instance.pending_requests.front().due_local_tick <= local_tick) {
			PendingRpcRequest pending = std::move(instance.pending_requests.front());
			instance.pending_requests.pop_front();
			ModuleRuntime* const module_ptr = &instance;
			std::string          label      = std::move(pending.label);
			queue_.schedule(
				global_tick, engine::Priority::CHILD_INTAKE, std::move(label),
				[this, module_ptr, local_tick, pending = std::move(pending)](engine::TaskContext& context) mutable {
					if (!module_ptr->accepting_rpc) return;
					const uint64_t         work_id = pending.work_id;
					const platform::Bucket bucket  = platform::request_bucket(pending.frame.cmd);
					module_ptr->rpc_work.emplace(work_id, std::move(pending));
					if (module_ptr->rpc_workers.enqueue(work_id, bucket).has_value()) {
						start_rpc_work(*module_ptr, work_id, context.tick(), local_tick);
					}
				});
		}
	}

	void Runtime::quiesce_module_for_stop(ModuleRuntime& instance, uint64_t local_tick,
										  std::optional<rpc::Frame> final_response) {
		instance.accepting_rpc = false;
		instance.pending_requests.clear();
		instance.pending_responses.clear();
		instance.staged_outbound.clear();
		instance.rpc_workers = platform::RpcWorkerPool{};
		instance.rpc_work.clear();

		const uint16_t module_id = instance.id.raw();
		for (auto& entry : pending_rpc_responses_) {
			auto& responses = entry.second;
			std::erase_if(responses,
						  [module_id](const ltl::RpcResponse& response) { return response.module == module_id; });
		}
		for (auto& entry : pending_rpc_events_) {
			auto& events = entry.second;
			std::erase_if(events, [module_id](const ltl::RpcEvent& event) { return event.module == module_id; });
		}

		if (final_response.has_value()) {
			final_response->handler_work_id = 0;
			instance.pending_responses.push_back(
				PendingRpcResponse{local_tick + RPC_TX_DELAY_LOCAL_TICKS, std::move(*final_response)});
		} else {
			instance.retire_pending = true;
		}
	}

	void Runtime::start_rpc_work(ModuleRuntime& instance, uint64_t work_id, uint64_t global_tick, uint64_t local_tick) {
		std::deque<uint64_t> work_to_start{work_id};
		while (!work_to_start.empty()) {
			const uint64_t current_work_id = work_to_start.front();
			work_to_start.pop_front();
			auto found = instance.rpc_work.find(current_work_id);
			if (found == instance.rpc_work.end()) {
				throw std::logic_error("RPC worker started an unknown request");
			}

			PendingRpcRequest pending = std::move(found->second);
			instance.rpc_work.erase(found);
			std::optional<rpc::Frame> response = platform_->handle_request(
				instance.id, pending.frame, platform::ModuleTime{global_tick, local_tick}, pending.ingress);
			if (!response) {
				if (platform_->lifecycle(instance.id) == platform::Lifecycle::STOPPED) {
					quiesce_module_for_stop(instance, local_tick, std::nullopt);
					return;
				}
				for (const uint64_t started : instance.rpc_workers.complete(current_work_id)) {
					work_to_start.push_back(started);
				}
				continue;
			}

			rpc::Frame answer             = std::move(response.value());
			answer.observation_request_id = pending.observation_id;
			if (answer.stop_after_delivery) {
				quiesce_module_for_stop(instance, local_tick, std::move(answer));
				return;
			}
			answer.handler_work_id = current_work_id;
			instance.pending_responses.push_back(
				PendingRpcResponse{local_tick + RPC_TX_DELAY_LOCAL_TICKS, std::move(answer)});
		}
	}

	void Runtime::schedule_tick_tasks() {
		const uint64_t tick = global_tick_;

		queue_.schedule(tick, engine::Priority::WORLD_ADVANCE, "world",
						[this](engine::TaskContext&) { world_.advance_tick(); });

		queue_.schedule(tick, engine::Priority::TRACE_FINALIZE, "trace", [this](engine::TaskContext&) {
			if (current_observation_) trace_observation(*current_observation_);
		});

		queue_.schedule(tick, engine::Priority::MONITORING, "supervision", [this, tick](engine::TaskContext&) {
			for (ModuleRuntime& instance : modules_) {
				if (instance.active && instance.retire_pending) retire_module(instance.id, tick);
			}
			for (const module::ModuleId id : platform_->check_supervision(tick)) {
				retire_module(id, tick);
			}
			for (const platform::Diagnostic& diagnostic : platform_->take_diagnostics()) {
				if (trace_.enabled()) {
					trace_.write(trace::Entry("diagnostic")
									 .field("tick", diagnostic.tick)
									 .field("module", static_cast<uint64_t>(diagnostic.module.raw()))
									 .field("kind", diagnostic.kind)
									 .field("bucket", platform::to_string(diagnostic.bucket))
									 .field("count", static_cast<uint64_t>(diagnostic.count))
									 .field("limit", static_cast<uint64_t>(diagnostic.limit)));
				}
				std::cerr << "tick " << diagnostic.tick << ": module " << static_cast<unsigned>(diagnostic.module.raw())
						  << " " << diagnostic.kind << " on the " << platform::to_string(diagnostic.bucket)
						  << " bucket";
				if (diagnostic.limit > 0) {
					std::cerr << " (" << diagnostic.count << "/" << diagnostic.limit << ")";
				}
				std::cerr << std::endl;
			}
			if (property_observer_ && (trace_.enabled() || !property_monitors_.empty())) {
				std::vector<ltl::RpcRequest>  requests;
				std::vector<ltl::RpcResponse> responses;
				std::vector<ltl::RpcEvent>    events;
				if (auto found = pending_rpc_requests_.find(tick); found != pending_rpc_requests_.end()) {
					requests = std::move(found->second);
					pending_rpc_requests_.erase(found);
				}
				if (auto found = pending_rpc_responses_.find(tick); found != pending_rpc_responses_.end()) {
					responses = std::move(found->second);
					pending_rpc_responses_.erase(found);
				}
				if (auto found = pending_rpc_events_.find(tick); found != pending_rpc_events_.end()) {
					events = std::move(found->second);
					pending_rpc_events_.erase(found);
				}
				// Which modules' code actually advanced on this tick. A module whose local clock
				// skipped the tick is still physically observable, but `next` must not discharge an
				// obligation against a step it never took.
				std::map<uint16_t, bool> module_stepped;
				for (const ModuleRuntime& instance : modules_) {
					module_stepped.emplace(instance.id.raw(), instance.tick_granted);
				}
				current_observation_ =
					property_observer_->observe(tick, world_, std::move(requests), std::move(responses),
												std::move(events), std::move(module_stepped));
				const ltl::Observation& observation = *current_observation_;
				// The evaluators only capture the observation, so every monitor shares them this tick.
				const ltl::AtomEvaluator evaluate = [&observation](const ltl::Formula& atom) {
					return observation.evaluate(atom);
				};
				const ltl::AtomValueRenderer actual = [&observation](const ltl::Formula& atom) {
					return observation.actual_value(atom);
				};
				const ltl::DomainEvaluator domains = [&observation](const ltl::Value* domain, ltl::ValueType type,
																	const ltl::DomainVisitor& visit) {
					observation.visit_domain(domain, type, visit);
				};
				const ltl::StepEvaluator stepped = [&observation](uint16_t module) {
					return observation.stepped(module);
				};
				const ltl::InputTracker inputs{
					&property_scratch_reads_,
					[&observation](const std::vector<uint32_t>& channels, uint64_t since) {
						return observation.changed_since(channels, since);
					},
					tick,
					[&observation](const ltl::Formula&                                  atom,
								   const std::vector<std::pair<std::string, uint64_t>>& bindings) {
						return observation.evaluate(atom, bindings);
					},
					[&observation](std::vector<uint32_t>& channels) { observation.deduplicate(channels); }};
				wake_properties(observation);
				for (std::size_t word = 0; word < property_bitset_words_; word++) {
					uint64_t ready = property_ready_[word];
					while (ready) {
						const std::size_t index = word * 64 + std::countr_zero(ready);
						ready &= ready - 1;
						if (tick > scenario_duration_tick_) continue;
						const ltl::PropertyDeclaration& declaration = properties_->properties[index];
						const uint64_t from_tick = declaration.scope ? declaration.scope->from_tick : 0;
						const uint64_t until_tick =
							declaration.scope ? declaration.scope->until_tick : scenario_duration_tick_;
						if (tick < from_tick || tick > until_tick) continue;

						if (property_decisive_ticks_[index]) continue;

						ltl::Monitor& monitor = property_monitors_[index];
						if (monitor.verdict() == ltl::Verdict::PENDING) {
							PropertySchedule& schedule = property_schedules_[index];
							// The scheduler has certified every skipped tick as an unchanged self-loop.
							if (monitor.next_tick() > tick) throw std::logic_error("LTL scheduler moved backwards");
							monitor.repeat(tick - monitor.next_tick());
							if (schedule.dirty) schedule.revision = ++next_property_revision_;
							property_scratch_reads_.clear();
							observation.track(&property_scratch_reads_);
							(void)monitor.observe(evaluate, actual, domains, stepped, schedule.revision, &inputs);
							observation.track(nullptr);
							schedule.dirty = false;
							if (monitor.progressed()) {
								observation.deduplicate(property_scratch_reads_);
								subscribe_property(index, property_scratch_reads_);
								schedule.reads.swap(property_scratch_reads_);
							}
							std::optional<uint64_t> next_wakeup;
							if (tick == until_tick) (void)monitor.finish(actual);
							else if (const auto boundary = monitor.repeats_until()) {
								const uint64_t wakeup = std::min(*boundary, until_tick);
								if (wakeup > tick + 1) {
									property_ready_[word] &= ~(uint64_t{1} << (index % 64));
									next_wakeup = wakeup;
								}
							}
							if (schedule.wakeup && (!next_wakeup || (*schedule.wakeup)->first != *next_wakeup)) {
								property_wakeups_.erase(*schedule.wakeup);
								schedule.wakeup.reset();
							}
							if (next_wakeup && !schedule.wakeup)
								schedule.wakeup = property_wakeups_.emplace(*next_wakeup, index);
						}
						if (monitor.verdict() != ltl::Verdict::PENDING) {
							property_decisive_ticks_[index] = tick;
							subscribe_property(index, {});
							property_schedules_[index].reads.clear();
							property_ready_[word] &= ~(uint64_t{1} << (index % 64));
							if (trace_.enabled()) {
								trace::Entry entry("property_verdict");
								entry.field("tick", tick)
									.field("name", declaration.name)
									.field("verdict", std::string(verdict_name(monitor.verdict())));
								if (monitor.failure()) {
									entry.field("formula", monitor.failure()->formula)
										.field("obligation", monitor.failure()->obligation)
										.field("fact_count", static_cast<uint64_t>(monitor.failure()->facts.size()));
									if (monitor.failure()->obligation_created_tick) {
										entry.field("obligation_created_tick",
													*monitor.failure()->obligation_created_tick);
									}
									if (monitor.failure()->deadline_tick) {
										entry.field("deadline_tick", *monitor.failure()->deadline_tick);
									}
								}
								trace_.write(entry);
								if (monitor.failure()) {
									for (const ltl::AtomFact& fact : monitor.failure()->facts) {
										trace::Entry fact_entry("property_fact");
										fact_entry.field("tick", tick)
											.field("name", declaration.name)
											.field("expression", fact.expression)
											.field("value", fact.value);
										if (fact.actual) fact_entry.field("actual", *fact.actual);
										trace_.write(fact_entry);
									}
								}
							}
						}
					}
				}
			}
		});

		queue_.schedule(tick, engine::Priority::PLATFORM_OUTBOUND, "outbound", [this, tick](engine::TaskContext&) {
			platform_->advance(tick);
			for (ModuleRuntime& instance : modules_) {
				if (!instance.active) continue;
				// Keep deliveries staged until this module receives another grant.
				if (!instance.staged_outbound.empty()) continue;
				for (rpc::Frame& frame : platform_->take_outbound(instance.id, QSIM_MAX_BATCH_BYTES)) {
					if (trace_.enabled() || !property_monitors_.empty()) {
						if (frame.type == MT_RESP && frame.observation_request_id != 0) {
							pending_rpc_responses_[tick].push_back(ltl::RpcResponse{
								frame.observation_request_id, frame.observation_request_id, tick, instance.id.raw(),
								frame.cmd, frame.call_id, frame.status, frame.payload});
						} else if (frame.type == MT_EVENT) {
							pending_rpc_events_[tick].push_back(ltl::RpcEvent{
								next_rpc_observation_id_++, tick, instance.id.raw(), frame.cmd, frame.payload});
						}
					}
					instance.staged_outbound.push_back(std::move(frame));
				}
			}
		});
	}

	void Runtime::stop() {
		if (!running_ && modules_.empty()) return;
		running_ = false;
		if (trace_.enabled()) {
			trace_.write(trace::Entry("run_complete").field("tick", global_tick_));
			trace_.flush();
		}
		if (backend_) backend_->stop();
		backend_.reset();
		modules_.clear();
		grants_.clear();
	}
} // namespace sim
