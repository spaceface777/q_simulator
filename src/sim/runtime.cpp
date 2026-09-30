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
#include <sched.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

#include "../ltl/checker.h"
#include "../module/module.h"
#include "../seed/seed.h"
#include "../settings.h"
#include "../trace/trace.h"
#include "../world/world.h"
#include "runahead.h"

namespace sim {

	namespace {

		// How much of the previous speed measurement survives into the next.
		constexpr double SPEED_SMOOTHING = 0.9;

		// Below this shortfall, treat it as measurement noise rather than being behind.
		constexpr double BEHIND_THRESHOLD = 0.9;

		// A log message requires a sustained shortfall; the GUI can still show smaller fluctuations.
		constexpr double WARNING_THRESHOLD = 0.95;

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

		uint64_t splitmix_next(uint64_t& state) {
			state += 0x9E3779B97F4A7C15ULL;
			uint64_t value = state;
			value          = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
			value          = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
			return value ^ (value >> 31);
		}

	} // namespace

	void pin_to_one_cpu() noexcept {
		const int cpu = ::sched_getcpu();
		if (cpu < 0) {
			std::cerr << "Could not select a CPU for lockstep affinity: " << std::strerror(errno) << std::endl;
			return;
		}
		if (cpu >= CPU_SETSIZE) {
			std::cerr << "Could not pin lockstep simulation: CPU " << cpu << " is outside CPU_SETSIZE" << std::endl;
			return;
		}

		cpu_set_t selected;
		CPU_ZERO(&selected);
		CPU_SET(cpu, &selected);
		if (::sched_setaffinity(0, sizeof(selected), &selected) != 0) {
			std::cerr << "Could not pin lockstep simulation to CPU " << cpu << ": " << std::strerror(errno)
					  << std::endl;
			return;
		}
		std::cout << "Pinned lockstep simulation to CPU " << cpu << std::endl;
	}

	Runtime::Runtime(uint64_t seed, std::chrono::milliseconds connect_timeout,
					 std::chrono::milliseconds activation_timeout, uint32_t tick_batch, uint32_t runahead_cap,
					 bool selective_sync)
		: seed_(seed), connect_timeout_(connect_timeout), activation_timeout_(activation_timeout),
		  tick_batch_(tick_batch), runahead_cap_(runahead_cap), selective_sync_(selective_sync),
		  active_modules_(settings::get_active_modules()), queue_(seed) {
		if (active_modules_.empty()) throw std::runtime_error("No active modules in layout.toml");
		for (const module::ModuleId id : active_modules_) world_.add_module(id);
		property_observer_.emplace(active_modules_, world_);
		const char* stats   = std::getenv("QSIM_RUNAHEAD_STATS");
		sync_stats_enabled_ = stats != nullptr && std::strcmp(stats, "0") != 0;
		const char* modules = std::getenv("QSIM_RUNAHEAD_STATS_MODULES");
		sync_stats_modules_ = modules != nullptr && std::strcmp(modules, "0") != 0;
	}

	Runtime::~Runtime() {
		stop();
	}

	void Runtime::start(std::optional<uint64_t> last_tick) {
		if (tick_batch_ == 0 || tick_batch_ > MAX_TICK_BATCH) {
			throw std::invalid_argument("Tick batch must be between 1 and " + std::to_string(MAX_TICK_BATCH));
		}
		if (runahead_cap_ > MAX_RUNAHEAD_TICKS) {
			throw std::invalid_argument("Exact runahead cap must be between 1 and " +
										std::to_string(MAX_RUNAHEAD_TICKS));
		}
		if (runahead_cap_ != 0 && tick_batch_ != 1) {
			throw std::invalid_argument("Exact runahead requires --tick-batch 1");
		}

		control_.emplace();
		childsync::Control& control = control_->get();

		// Published before any application launches, so a stale binary fails at startup with a
		// clear message rather than silently reading the segment at the wrong offsets.
		childsync::store_release(control.abi, QUERCUS_SHM_ABI);
		childsync::store_relaxed(control.selective_enabled, selective_sync_ ? 1U : 0U);

		// All children participate initially. Heartbeat supervision can reduce this before a later
		// generation after the failed child has been reaped.
		childsync::store_relaxed(control.expected, static_cast<uint32_t>(active_modules_.size()));
		control_->mapping().keep_across_exec();

		for (const module::ModuleId id : active_modules_) {
			Child child;
			child.id                          = id;
			child.scheduler_index             = static_cast<uint8_t>(children_.size());
			control.scheduler_index[id.raw()] = child.scheduler_index;
			seed::SplitMix64 timing(seed::derive(seed_, "module-clock/" + std::to_string(id.raw())));
			const double     start_ms =
				settings::get_setting<double>("module_start_offset_mean_ms") +
				settings::get_setting<double>("module_start_offset_sd_ms") * standard_normal(timing);
			child.start_tick     = start_ms <= 0.0 ? 0u : static_cast<uint64_t>(std::llround(start_ms));
			const double drift   = settings::get_setting<double>("module_clock_drift_sd_ppm") * standard_normal(timing);
			child.clock_rate_ppm = std::max<int64_t>(1, 1'000'000 + static_cast<int64_t>(std::llround(drift)));
			const double skip_ppm = settings::get_setting<double>("module_tick_skip_mean_ppm") +
									settings::get_setting<double>("module_tick_skip_sd_ppm") * standard_normal(timing);
			child.tick_skip_ppm   = static_cast<uint32_t>(std::clamp(std::llround(skip_ppm), 0LL, 1'000'000LL));
			child.tick_skip_rng   = seed::derive(seed_, "module-clock-skips/" + std::to_string(id.raw()));
			child.segment.emplace();
			child.segment->mapping().keep_across_exec();

			childproc::LaunchSpec spec;
			spec.id         = id;
			spec.binary     = settings::get_module_binary(id);
			spec.control_fd = control_->mapping().fd();
			spec.mailbox_fd = child.segment->mapping().fd();
			spec.seed       = seed_;

			std::cout << "Starting module " << static_cast<unsigned>(id.raw()) << " from " << spec.binary << " at tick "
					  << child.start_tick << " with clock " << child.clock_rate_ppm << " ppm" << std::endl;
			std::cout << "  tick skip chance " << child.tick_skip_ppm << " ppm" << std::endl;
			if (trace_.enabled()) {
				trace_.write(trace::Entry("module_clock")
								 .field("module", static_cast<uint64_t>(id.raw()))
								 .field("start_tick", child.start_tick)
								 .field("rate_ppm", child.clock_rate_ppm)
								 .field("skip_ppm", static_cast<uint64_t>(child.tick_skip_ppm)));
			}
			child.process = childproc::launch(spec);
			children_.push_back(std::move(child));
		}

		running_ = true;
		platform_.emplace(world_);

		// The segments exist before any application launches, so mapping order does not matter.
		const Clock::time_point connect_deadline = Clock::now() + connect_timeout_;
		std::size_t             mapped           = 0;
		while (mapped < children_.size()) {
			if (Clock::now() > connect_deadline) {
				stop();
				throw std::runtime_error("Timed out waiting for module applications to connect");
			}

			mapped = 0;
			for (const Child& child : children_) {
				if (control.ready[child.id.raw()] != 0) {
					mapped++;
				}
			}

			if (mapped < children_.size()) {
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}

		std::cout << "Waiting for applications to activate" << std::endl;

		// An application activates by sending its first heartbeat, which requires being ticked first.
		const Clock::time_point activation_deadline = Clock::now() + activation_timeout_;
		while (true) {
			const bool all_active = std::all_of(children_.begin(), children_.end(), [this](const Child& child) {
				return platform_->is_activated(child.id) ||
					   platform_->lifecycle(child.id) == platform::Lifecycle::STOPPED;
			});
			if (all_active) {
				break;
			}
			if (Clock::now() > activation_deadline) {
				stop();
				throw std::runtime_error("Timed out waiting for module applications to activate");
			}
			if (selective_sync_) step_selective(1);
			else step(1);
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
		entry.field("modules", static_cast<uint64_t>(active_modules_.size()));
		entry.field("tick_batch_requested", static_cast<uint64_t>(tick_batch_))
			.field("tick_batch_effective", static_cast<uint64_t>(tick_batch_));
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
			if (global_tick_ > from_tick && property_monitors_[index].verdict() == ltl::Verdict::FALSE) return true;
		}
		return false;
	}

	std::size_t Runtime::properties_pending() const noexcept {
		std::size_t pending = 0;
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			if (global_tick_ <= from_tick || property_monitors_[index].verdict() == ltl::Verdict::PENDING) pending++;
		}
		return pending;
	}

	std::size_t Runtime::properties_unresolved() const noexcept {
		std::size_t unresolved = 0;
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			if (global_tick_ > from_tick && property_monitors_[index].verdict() == ltl::Verdict::UNRESOLVED)
				unresolved++;
		}
		return unresolved;
	}

	std::size_t Runtime::properties_failed() const noexcept {
		std::size_t failed = 0;
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			if (global_tick_ > from_tick && property_monitors_[index].verdict() == ltl::Verdict::FALSE) failed++;
		}
		return failed;
	}

	std::vector<PropertyResult> Runtime::property_results() const {
		std::vector<PropertyResult> results;
		results.reserve(property_monitors_.size());
		for (std::size_t index = 0; index < property_monitors_.size(); index++) {
			const ltl::PropertyDeclaration& declaration = properties_->properties[index];
			const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
			const ltl::Verdict              verdict =
				global_tick_ <= from_tick ? ltl::Verdict::PENDING : property_monitors_[index].verdict();
			const ltl::Monitor& monitor = property_monitors_[index];
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
		if (tick_batch_ > 1) {
			// Every batched grant has the same relative-slot layout. Keep a short tail for the
			// next frame rather than using it as a smaller batch and misaligning inbound frames.
			granted -= granted % tick_batch_;
		}
		budget_us_ -= granted * TICK_DURATION.count();
		if (budget_us_ < 0) {
			budget_us_ = 0;
		}

		const Clock::time_point started     = Clock::now();
		const Clock::time_point limit       = started + deadline;
		const uint64_t          tick_before = global_tick_;

		for (int64_t index = 0; index < granted;) {
			const uint32_t remaining = static_cast<uint32_t>(std::min<int64_t>(granted - index, UINT32_MAX));
			const uint32_t batch     = runahead_cap_ == 0 ? tick_batch_ : std::min(runahead_cap_, remaining);
			const uint32_t advanced =
				selective_sync_ ? step_selective(tick_batch_) : (runahead_cap_ == 0 ? step(batch) : step_exact(batch));
			index += advanced;

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

	std::vector<Runtime::FutureGrant> Runtime::plan_future_grants(uint32_t ticks) const {
		std::vector<FutureGrant> plans(children_.size());
		for (std::size_t child_index = 0; child_index < children_.size(); child_index++) {
			const Child& child = children_[child_index];
			if (!child.active) continue;

			uint64_t skip_rng        = child.tick_skip_rng;
			int64_t  clock_remainder = child.clock_remainder;
			for (uint32_t index = 0; index < ticks; index++) {
				FutureGrant&   plan     = plans[child_index];
				const uint64_t tick     = global_tick_ + index;
				const bool     started  = tick >= child.start_tick;
				const bool     eligible = started && !platform_->is_paused(child.id, tick);
				const bool     skipped  = eligible && child.tick_skip_ppm != 0 &&
										  (splitmix_next(skip_rng) % 1'000'000u) < child.tick_skip_ppm;
				plan.skipped[index]     = skipped;
				plan.granted[index]     = eligible && !skipped;
				if (!plan.granted[index]) continue;

				clock_remainder += TICK_DURATION.count() * child.clock_rate_ppm;
				const int64_t delta = clock_remainder / 1'000'000;
				clock_remainder %= 1'000'000;
				plan.delta_us[index] = static_cast<uint32_t>(std::max<int64_t>(1, delta));
			}
		}
		return plans;
	}

	uint32_t Runtime::child_safe_horizon(const std::vector<FutureGrant>& grants, uint32_t max_ticks,
										 BoundaryProbe* probe) const {
		const childsync::Control& control = control_->get();
		uint32_t                  safe    = max_ticks;
		if (probe != nullptr) probe->probed_ticks = max_ticks;

		for (std::size_t child_index = 0; child_index < children_.size(); child_index++) {
			const Child& child = children_[child_index];
			if (!child.active) continue;

			const FutureGrant& plan     = grants[child_index];
			const bool         advances = std::any_of(plan.granted.begin(), plan.granted.begin() + max_ticks,
													  [](bool granted) { return granted; });
			if (!advances) continue;

			const quercus_runahead_certificate_t& certificate = control.certificate[child.id.raw()];
			if (childsync::load_acquire(certificate.valid) == 0 || certificate.generation != generation_ ||
				certificate.next_event_us <= certificate.now_us) {
				if (probe == nullptr) return 0;
				probe->child_horizon[child.id.raw()] = 0;
				safe                                 = 0;
				continue;
			}
			const uint32_t horizon = runahead::safe_prefix(
				certificate.now_us, certificate.next_event_us,
				std::span<const bool>(plan.granted.data(), plan.granted.size()), probe == nullptr ? safe : max_ticks,
				std::span<const uint32_t>(plan.delta_us.data(), plan.delta_us.size()));
			if (probe != nullptr) {
				probe->child_horizon[child.id.raw()] = horizon;
				probe->child_proven[child.id.raw()]  = true;
			}
			safe = std::min(safe, horizon);
			if (safe == 0 && probe == nullptr) return 0;
		}
		return safe;
	}

	uint32_t Runtime::parent_safe_horizon(uint32_t max_ticks, BoundaryProbe* probe) const {
		for (const Child& child : children_) {
			if (!child.staged_outbound.empty() || !child.pending_requests.empty() || !child.pending_responses.empty() ||
				!child.rpc_work.empty() || child.rpc_workers.occupied() != 0 || child.rpc_workers.queued() != 0 ||
				child.retire_pending || child.retire_after_ack) {
				if (probe != nullptr) {
					probe->limit = BoundaryLimit::CONSERVATIVE_FALLBACK;
					if (!child.staged_outbound.empty() || !child.pending_requests.empty() ||
						!child.pending_responses.empty() || !child.rpc_work.empty() ||
						child.rpc_workers.occupied() != 0 || child.rpc_workers.queued() != 0) {
						probe->engine_delivery[child.id.raw()] = true;
						probe->limit                           = BoundaryLimit::ENGINE_DELIVERY;
					}
				}
				return 0;
			}
		}

		const std::optional<uint64_t> next_task = queue_.next_due_tick();
		if (next_task && *next_task <= global_tick_) {
			if (probe != nullptr) probe->limit = BoundaryLimit::CONSERVATIVE_FALLBACK;
			return 0;
		}
		if (next_task) {
			const uint32_t task_horizon = static_cast<uint32_t>(
				std::min<uint64_t>(max_ticks, *next_task > global_tick_ ? *next_task - global_tick_ : 0));
			if (task_horizon < max_ticks && probe != nullptr) {
				probe->limit = BoundaryLimit::CONSERVATIVE_FALLBACK;
			}
			max_ticks = task_horizon;
		}
		if (max_ticks == 0) return 0;

		try {
			world::State      future_world    = world_;
			platform::Manager future_platform = platform_->clone_for_lookahead(future_world);
			for (uint32_t index = 0; index < max_ticks; index++) {
				const uint64_t tick = global_tick_ + index;
				future_platform.prepare_tick(tick);
				future_world.advance_tick();
				future_platform.advance(tick);

				bool produced_delivery = false;
				for (const Child& child : children_) {
					if (!child.active) continue;
					if (!future_platform.take_outbound(child.id, childsync::MAILBOX_CAPACITY).empty()) {
						produced_delivery = true;
						if (probe != nullptr) probe->engine_delivery[child.id.raw()] = true;
					}
				}
				if (produced_delivery) {
					// The frame is produced after this logical tick and is child-visible at a later
					// handoff. This tick itself is still part of the certified quiet prefix.
					if (probe != nullptr) probe->limit = BoundaryLimit::ENGINE_DELIVERY;
					return index + 1;
				}

				if (!future_platform.check_supervision(tick).empty()) {
					if (probe != nullptr) probe->limit = BoundaryLimit::CONSERVATIVE_FALLBACK;
					return index + 1;
				}
				(void)future_platform.take_diagnostics();
			}
		} catch (const std::exception&) {
			if (probe != nullptr) probe->limit = BoundaryLimit::CONSERVATIVE_FALLBACK;
			return 0;
		}

		return max_ticks;
	}

	void Runtime::grant_runahead(const std::vector<FutureGrant>& grants, uint32_t ticks) {
		childsync::Control& control = control_->get();
		control.grant_mode          = QUERCUS_GRANT_EXACT_RUNAHEAD;
		control.runahead_ticks      = ticks;

		for (std::size_t child_index = 0; child_index < children_.size(); child_index++) {
			Child& child = children_[child_index];
			for (uint32_t word = 0; word < QUERCUS_RUNAHEAD_MASK_WORDS; word++) {
				control.runahead[child.id.raw()].active[word] = 0;
			}
			for (uint32_t index = 0; index < ticks; index++) {
				if (grants[child_index].granted[index]) {
					control.runahead[child.id.raw()].active[index / 64U] |= UINT64_C(1) << (index % 64U);
				}
				control.runahead[child.id.raw()].delta_us[index] = grants[child_index].delta_us[index];
			}
			for (uint32_t index = 0; index < MAX_TICK_BATCH; index++) {
				childsync::mailbox_clear(child.segment->get().to_parent[index]);
				childsync::mailbox_clear(child.segment->get().to_child[index]);
			}
		}

		platform_->prepare_tick(global_tick_);
		childsync::store_relaxed(control.done_count, 0);
		const uint32_t expected =
			static_cast<uint32_t>(std::ranges::count_if(children_, [](const Child& child) { return child.active; }));
		childsync::store_relaxed(control.expected, expected);
		generation_++;
		childsync::store_release(control.generation, generation_);
		childsync::futex_wake(control.generation, INT_MAX);
	}

	void Runtime::capture_active_modules(BoundaryProbe& probe) const {
		for (const Child& child : children_) {
			if (child.active) probe.active[child.id.raw()] = true;
		}
	}

	void Runtime::mark_required_child_wakes(BoundaryProbe& probe, uint32_t boundary_ticks) const {
		for (const Child& child : children_) {
			const uint8_t id = child.id.raw();
			if (probe.child_proven[id] && probe.child_horizon[id] <= boundary_ticks &&
				probe.child_horizon[id] < probe.probed_ticks) {
				probe.child_wake[id] = true;
			}
		}
	}

	void Runtime::record_synchronization(uint32_t logical_ticks, BoundaryProbe probe) {
		if (!sync_stats_enabled_) return;

		bool child_required  = false;
		bool engine_required = false;
		for (const Child& child : children_) {
			const uint8_t id = child.id.raw();
			if (child.active && !child.staged_outbound.empty()) probe.engine_delivery[id] = true;
			child_required  = child_required || probe.child_wake[id];
			engine_required = engine_required || probe.engine_delivery[id];
		}
		if (child_required) {
			probe.limit = BoundaryLimit::CHILD_WAKE;
		} else if (engine_required) {
			probe.limit = BoundaryLimit::ENGINE_DELIVERY;
		}

		sync_stats_.logical_ticks += logical_ticks;
		sync_stats_.boundaries++;
		sync_stats_.ticks_per_boundary[std::min<uint32_t>(logical_ticks, MAX_RUNAHEAD_TICKS)]++;
		sync_stats_.limits[static_cast<std::size_t>(probe.limit)]++;

		uint32_t wake_set_size = 0;
		for (const Child& child : children_) {
			const uint8_t id = child.id.raw();
			if (probe.active[id]) {
				sync_stats_.current_child_wakeups++;
				sync_stats_.current_module_wakes[id]++;
			}
			if (probe.active[id] && (probe.child_wake[id] || probe.engine_delivery[id])) {
				wake_set_size++;
				sync_stats_.required_child_wakeups++;
				sync_stats_.required_module_wakes[id]++;
			}
		}
		sync_stats_.required_wake_set_sizes[wake_set_size]++;
	}

	bool Runtime::selective_child_due(const Child& child, const FutureGrant& grant, uint32_t batch_ticks) const {
		if (!child.active) return false;
		if (!platform_->is_activated(child.id)) return true;
		if (!child.staged_outbound.empty() || !child.pending_requests.empty() || !child.pending_responses.empty() ||
			!child.rpc_work.empty() || child.rpc_workers.occupied() != 0 || child.rpc_workers.queued() != 0 ||
			child.retire_pending || child.retire_after_ack)
			return true;
		const childsync::Control&             control     = control_->get();
		const quercus_runahead_certificate_t& certificate = control.certificate[child.id.raw()];
		if (childsync::load_acquire(certificate.valid) == 0 || certificate.generation != child.wake_seq ||
			certificate.next_event_us <= certificate.now_us)
			return true;
		if (child.deferred_grants.size() + batch_ticks > MAX_RUNAHEAD_TICKS) return true;
		if (certificate.next_event_us == UINT64_MAX) return false;

		uint64_t elapsed = 0;
		for (const DeferredGrant& deferred : child.deferred_grants) {
			if (deferred.granted) elapsed += deferred.delta_us;
		}
		for (uint32_t index = 0; index < batch_ticks; index++) {
			if (grant.granted[index]) elapsed += grant.delta_us[index];
		}
		return elapsed >= certificate.next_event_us - certificate.now_us;
	}

	void Runtime::grant_selective(const std::vector<FutureGrant>& grants, uint32_t batch_ticks,
								  const std::vector<bool>& selected) {
		childsync::Control& control = control_->get();
		generation_++;
		uint32_t expected = 0;

		for (std::size_t child_index = 0; child_index < children_.size(); child_index++) {
			Child& child            = children_[child_index];
			child.selected          = child.active && selected[child_index];
			child.grant_mask        = 0;
			child.skipped_mask      = 0;
			const FutureGrant& plan = grants[child_index];

			// Commit exactly the same clock RNG and remainder transitions as the reference batch path.
			for (uint32_t index = 0; index < batch_ticks; index++) {
				const uint16_t slot    = static_cast<uint16_t>(1U << index);
				const bool     started = global_tick_ + index >= child.start_tick;
				const bool eligible = child.active && started && !platform_->is_paused(child.id, global_tick_ + index);
				bool       skipped  = false;
				if (eligible && child.tick_skip_ppm != 0)
					skipped = (splitmix_next(child.tick_skip_rng) % 1'000'000u) < child.tick_skip_ppm;
				if (skipped != plan.skipped[index] || (eligible && !skipped) != plan.granted[index])
					throw std::logic_error("Selective grant diverged from planned clock state");
				if (skipped) child.skipped_mask |= slot;
				if (!plan.granted[index]) continue;
				child.grant_mask |= slot;
				child.clock_remainder += TICK_DURATION.count() * child.clock_rate_ppm;
				const int64_t delta = child.clock_remainder / 1'000'000;
				child.clock_remainder %= 1'000'000;
				if (static_cast<uint32_t>(std::max<int64_t>(1, delta)) != plan.delta_us[index])
					throw std::logic_error("Selective grant delta diverged from planned clock state");
				control.delta_us[child.id.raw()][index] = plan.delta_us[index];
			}
			control.active_mask[child.id.raw()] = child.selected ? child.grant_mask : 0;

			if (!child.selected) {
				for (uint32_t index = 0; index < batch_ticks; index++)
					child.deferred_grants.push_back({plan.granted[index], plan.delta_us[index]});
				continue;
			}

			expected++;
			quercus_selective_grant_t& selective = control.selective[child.id.raw()];
			selective.generation                 = generation_;
			selective.prefix_ticks               = static_cast<uint32_t>(child.deferred_grants.size());
			selective.batch_ticks                = batch_ticks;
			std::fill(std::begin(selective.prefix_active), std::end(selective.prefix_active), UINT64_C(0));
			for (uint32_t index = 0; index < selective.prefix_ticks; index++) {
				const DeferredGrant deferred = child.deferred_grants[index];
				if (deferred.granted) selective.prefix_active[index / 64U] |= UINT64_C(1) << (index % 64U);
				selective.prefix_delta_us[index] = deferred.delta_us;
			}
			child.deferred_grants.clear();
			for (uint32_t index = 0; index < MAX_TICK_BATCH; index++) {
				childsync::mailbox_clear(child.segment->get().to_parent[index]);
				childsync::mailbox_clear(child.segment->get().to_child[index]);
			}
			if (child.grant_mask != 0) {
				const uint32_t      first_active = std::countr_zero(child.grant_mask);
				childsync::Mailbox& mailbox      = child.segment->get().to_child[first_active];
				while (!child.staged_outbound.empty()) {
					const rpc::Frame&          frame = child.staged_outbound.front();
					const std::vector<uint8_t> bytes = rpc::encode(frame);
					if (childsync::mailbox_append(mailbox, bytes.data(), bytes.size()) != childsync::MailboxStatus::OK)
						break;
					const uint64_t work_id        = frame.handler_work_id;
					const bool     stop_after_ack = frame.stop_after_delivery;
					child.staged_outbound.pop_front();
					if (stop_after_ack) child.retire_after_ack = true;
					if (work_id != 0)
						for (const uint64_t ready : child.rpc_workers.complete(work_id))
							start_rpc_work(child, ready, global_tick_, module_ticks_[child.id.raw()]);
				}
			}
		}

		platform_->prepare_tick(global_tick_);
		childsync::store_relaxed(control.done_count, 0);
		childsync::store_relaxed(control.expected, expected);
		if (expected == 0) return;

		std::array<uint32_t, QUERCUS_FUTEX_BANKS> masks{};
		for (Child& child : children_) {
			if (!child.selected) continue;
			child.wake_seq++;
			childsync::store_release(control.wake_seq[child.id.raw()], child.wake_seq);
			const uint32_t index = child.scheduler_index;
			masks[index / 32U] |= UINT32_C(1) << (index % 32U);
		}
		for (uint32_t bank = 0; bank < QUERCUS_FUTEX_BANKS; bank++) {
			if (masks[bank] == 0) continue;
			const uint32_t sequence = childsync::load_acquire(control.futex_bank[bank]) + 1U;
			childsync::store_release(control.futex_bank[bank], sequence);
			(void)childsync::futex_wake_bitset(control.futex_bank[bank], INT_MAX, masks[bank]);
		}
	}

	uint32_t Runtime::step_selective(uint32_t batch_ticks) {
		const std::vector<FutureGrant> grants = plan_future_grants(batch_ticks);
		std::vector<bool>              selected(children_.size(), false);
		uint32_t                       selected_count = 0;
		for (std::size_t index = 0; index < children_.size(); index++) {
			selected[index] = selective_child_due(children_[index], grants[index], batch_ticks);
			if (selected[index]) selected_count++;
		}
		if (sync_stats_enabled_) {
			sync_stats_.logical_ticks += batch_ticks;
			selective_ticks_since_sync_ += batch_ticks;
			if (selected_count != 0) {
				sync_stats_.boundaries++;
				sync_stats_.ticks_per_boundary[std::min(selective_ticks_since_sync_, MAX_RUNAHEAD_TICKS)]++;
				sync_stats_.required_wake_set_sizes[selected_count]++;
				sync_stats_.current_child_wakeups += selected_count;
				sync_stats_.required_child_wakeups += selected_count;
				sync_stats_.limits[static_cast<std::size_t>(BoundaryLimit::CHILD_WAKE)]++;
				for (std::size_t index = 0; index < children_.size(); index++)
					if (selected[index]) {
						const uint8_t id = children_[index].id.raw();
						sync_stats_.current_module_wakes[id]++;
						sync_stats_.required_module_wakes[id]++;
					}
				selective_ticks_since_sync_ = 0;
			}
		}

		grant_selective(grants, batch_ticks, selected);
		collect_acknowledgements();
		for (Child& child : children_)
			if (child.selected && child.active && child.retire_after_ack) retire_module(child.id, global_tick_);

		uint32_t completed = 0;
		for (uint32_t batch_index = 0; batch_index < batch_ticks; batch_index++) {
			for (Child& child : children_) {
				const uint16_t slot = static_cast<uint16_t>(1U << batch_index);
				child.tick_granted  = child.active && (child.grant_mask & slot) != 0;
				if ((child.skipped_mask & slot) != 0 && trace_.enabled())
					trace_.write(trace::Entry("module_tick_skipped")
									 .field("tick", global_tick_)
									 .field("module", static_cast<uint64_t>(child.id.raw()))
									 .field("local_tick", module_ticks_[child.id.raw()]));
				if (!child.tick_granted) continue;
				const uint64_t local_tick = module_ticks_[child.id.raw()];
				if (child.selected) drain(child, global_tick_, local_tick, batch_index);
				release_local_rpc(child, global_tick_, local_tick);
				module_ticks_[child.id.raw()]++;
			}
			if (batch_index != 0) platform_->prepare_tick(global_tick_);
			schedule_tick_tasks(batch_index, batch_ticks);
			queue_.run_tick(global_tick_);
			global_tick_++;
			completed++;
			if (halted() || (!property_monitors_.empty() && properties_pending() == 0)) break;
		}
		// A sleeping child does not participate in the barrier. Poll all active processes once per
		// epoch so a failure cannot stay hidden until that child is selected again.
		for (Child& child : children_) {
			if (child.active && child.process.exit_status().has_value())
				throw std::runtime_error("Lost the connection to module " + std::to_string(child.id.raw()));
		}
		return completed;
	}

	uint32_t Runtime::step_exact(uint32_t max_ticks) {
		std::optional<BoundaryProbe> probe;
		if (sync_stats_enabled_) probe.emplace();
		BoundaryProbe* measured = probe ? &*probe : nullptr;
		if (measured != nullptr) {
			measured->limit = BoundaryLimit::HARD_LOOKAHEAD_CAP;
			capture_active_modules(*measured);
		}
		if (max_ticks < 2) {
			runahead_fallbacks_++;
			if (measured != nullptr) pending_strict_probe_ = std::move(*measured);
			return step(1);
		}

		const std::vector<FutureGrant> grants = plan_future_grants(max_ticks);
		uint32_t                       safe   = child_safe_horizon(grants, max_ticks, measured);
		if (safe < 2) {
			runahead_fallbacks_++;
			if (measured != nullptr) {
				mark_required_child_wakes(*measured, 1);
				measured->limit       = std::ranges::any_of(measured->child_wake, [](bool wake) { return wake; })
											? BoundaryLimit::CHILD_WAKE
											: BoundaryLimit::CONSERVATIVE_FALLBACK;
				pending_strict_probe_ = std::move(*measured);
			}
			return step(1);
		}
		const uint32_t child_safe  = safe;
		const uint32_t parent_safe = parent_safe_horizon(safe, measured);
		safe                       = std::min(safe, parent_safe);
		if (safe < 2) {
			runahead_fallbacks_++;
			if (measured != nullptr) {
				mark_required_child_wakes(*measured, 1);
				pending_strict_probe_ = std::move(*measured);
			}
			return step(1);
		}
		if (measured != nullptr) {
			mark_required_child_wakes(*measured, safe);
			if (child_safe == safe && std::ranges::any_of(measured->child_wake, [](bool wake) { return wake; })) {
				measured->limit = BoundaryLimit::CHILD_WAKE;
			} else if (parent_safe == safe && measured->limit != BoundaryLimit::ENGINE_DELIVERY &&
					   measured->limit != BoundaryLimit::CONSERVATIVE_FALLBACK) {
				measured->limit = BoundaryLimit::HARD_LOOKAHEAD_CAP;
			}
		}

		runahead_windows_++;
		runahead_ticks_ += safe;
		grant_runahead(grants, safe);
		collect_acknowledgements();
		for (const Child& child : children_) {
			for (uint32_t index = 0; index < MAX_TICK_BATCH; index++) {
				if (childsync::mailbox_count(child.segment->get().to_parent[index]) != 0) {
					throw std::logic_error("Module " + std::to_string(child.id.raw()) +
										   " emitted RPC inside a certified runahead interval");
				}
			}
		}

		uint32_t completed = 0;
		for (uint32_t index = 0; index < safe; index++) {
			for (std::size_t child_index = 0; child_index < children_.size(); child_index++) {
				Child&             child = children_[child_index];
				const FutureGrant& plan  = grants[child_index];
				child.tick_granted       = child.active && plan.granted[index];

				// Commit only this logical tick's clock RNG and drift state. If an LTL verdict ends
				// the interval early, no speculative parent RNG state leaks into the committed run.
				const bool started  = global_tick_ >= child.start_tick;
				const bool eligible = child.active && started && !platform_->is_paused(child.id, global_tick_);
				bool       skipped  = false;
				if (eligible && child.tick_skip_ppm != 0) {
					skipped = (splitmix_next(child.tick_skip_rng) % 1'000'000u) < child.tick_skip_ppm;
				}
				if (skipped != plan.skipped[index] || (eligible && !skipped) != plan.granted[index]) {
					throw std::logic_error("Exact runahead grant diverged from committed clock state");
				}
				if (skipped && trace_.enabled()) {
					trace_.write(trace::Entry("module_tick_skipped")
									 .field("tick", global_tick_)
									 .field("module", static_cast<uint64_t>(child.id.raw()))
									 .field("local_tick", module_ticks_[child.id.raw()]));
				}
				if (child.tick_granted) {
					child.clock_remainder += TICK_DURATION.count() * child.clock_rate_ppm;
					const int64_t delta = child.clock_remainder / 1'000'000;
					child.clock_remainder %= 1'000'000;
					if (static_cast<uint32_t>(std::max<int64_t>(1, delta)) != plan.delta_us[index]) {
						throw std::logic_error("Exact runahead delta diverged from committed clock state");
					}
					const uint64_t local_tick = module_ticks_[child.id.raw()];
					release_local_rpc(child, global_tick_, local_tick);
					module_ticks_[child.id.raw()]++;
				}
			}
			if (index != 0) platform_->prepare_tick(global_tick_);
			schedule_tick_tasks(index, safe);
			queue_.run_tick(global_tick_);
			global_tick_++;
			completed++;

			if (index + 1U < safe) {
				for (const Child& child : children_) {
					if (child.active && platform_->backlog(child.id) != 0) {
						throw std::logic_error("Parent produced an unpredicted handoff inside exact runahead");
					}
				}
			}
			if (halted() || (!property_monitors_.empty() && properties_pending() == 0)) break;
		}
		if (measured != nullptr) record_synchronization(completed, std::move(*measured));
		return completed;
	}

	uint32_t Runtime::step(uint32_t batch_ticks) {
		std::optional<BoundaryProbe> probe;
		if (sync_stats_enabled_) {
			probe.emplace();
			if (pending_strict_probe_) {
				*probe = std::move(*pending_strict_probe_);
				pending_strict_probe_.reset();
			} else {
				probe->limit =
					runahead_cap_ == 0 ? BoundaryLimit::HARD_LOOKAHEAD_CAP : BoundaryLimit::CONSERVATIVE_FALLBACK;
				capture_active_modules(*probe);
				if (runahead_cap_ != 0) {
					const std::vector<FutureGrant> grants = plan_future_grants(batch_ticks);
					(void)child_safe_horizon(grants, batch_ticks, &*probe);
					mark_required_child_wakes(*probe, batch_ticks);
				}
			}
		}
		// A batch advances application-local time first, then replays the parent one tick at a time.
		// Parent-produced frames therefore become visible only on a later batch; this is the explicit
		// relaxed-timing mode selected by --tick-batch.
		grant_tick(batch_ticks);
		collect_acknowledgements();
		for (Child& child : children_) {
			if (child.active && child.retire_after_ack) retire_module(child.id, global_tick_);
		}

		uint32_t completed = 0;
		for (uint32_t batch_index = 0; batch_index < batch_ticks; batch_index++) {
			for (Child& child : children_) {
				const uint16_t slot = static_cast<uint16_t>(1U << batch_index);
				child.tick_granted  = child.active && (child.grant_mask & slot) != 0;
				if ((child.skipped_mask & slot) != 0 && trace_.enabled()) {
					trace_.write(trace::Entry("module_tick_skipped")
									 .field("tick", global_tick_)
									 .field("module", static_cast<uint64_t>(child.id.raw()))
									 .field("local_tick", module_ticks_[child.id.raw()]));
				}
				if (child.tick_granted) {
					const uint64_t local_tick = module_ticks_[child.id.raw()];
					drain(child, global_tick_, local_tick, batch_index);
					release_local_rpc(child, global_tick_, local_tick);
					module_ticks_[child.id.raw()]++;
				}
			}
			if (batch_index != 0) {
				platform_->prepare_tick(global_tick_);
			}
			schedule_tick_tasks(batch_index, batch_ticks);
			queue_.run_tick(global_tick_);
			global_tick_++;
			completed++;
			if (halted() || (!property_monitors_.empty() && properties_pending() == 0)) {
				break;
			}
		}
		if (probe) record_synchronization(completed, std::move(*probe));
		return completed;
	}

	void Runtime::grant_tick(uint32_t batch_ticks) {
		childsync::Control& control = control_->get();

		control.grant_mode  = QUERCUS_GRANT_BATCH;
		control.batch_ticks = batch_ticks;

		for (Child& child : children_) {
			child.grant_mask   = 0;
			child.skipped_mask = 0;
			if (!child.active) {
				control.active_mask[child.id.raw()] = 0;
				continue;
			}
			for (uint32_t index = 0; index < batch_ticks; index++) {
				const uint16_t slot      = static_cast<uint16_t>(1U << index);
				const uint64_t slot_tick = global_tick_ + index;
				const bool     started   = slot_tick >= child.start_tick;
				const bool     eligible  = started && !platform_->is_paused(child.id, slot_tick);
				const bool     skipped   = eligible && child.tick_skip_ppm != 0 &&
										   (splitmix_next(child.tick_skip_rng) % 1'000'000u) < child.tick_skip_ppm;
				if (skipped) child.skipped_mask |= slot;
				if (!eligible || skipped) continue;

				child.grant_mask |= slot;
				child.clock_remainder += TICK_DURATION.count() * child.clock_rate_ppm;
				const int64_t delta = child.clock_remainder / 1'000'000;
				child.clock_remainder %= 1'000'000;
				control.delta_us[child.id.raw()][index] = static_cast<uint32_t>(std::max<int64_t>(1, delta));
			}
			control.active_mask[child.id.raw()] = child.grant_mask;

			for (uint32_t index = 0; index < MAX_TICK_BATCH; index++) {
				childsync::mailbox_clear(child.segment->get().to_parent[index]);
				childsync::mailbox_clear(child.segment->get().to_child[index]);
			}

			if (child.grant_mask != 0) {
				const uint32_t      first_active = std::countr_zero(child.grant_mask);
				childsync::Mailbox& mailbox      = child.segment->get().to_child[first_active];
				while (!child.staged_outbound.empty()) {
					const rpc::Frame&          frame = child.staged_outbound.front();
					const std::vector<uint8_t> bytes = rpc::encode(frame);
					if (childsync::mailbox_append(mailbox, bytes.data(), bytes.size()) != childsync::MailboxStatus::OK)
						break;

					const uint64_t completed_work_id = frame.handler_work_id;
					const bool     stop_after_ack    = frame.stop_after_delivery;
					child.staged_outbound.pop_front();
					if (stop_after_ack) child.retire_after_ack = true;
					if (completed_work_id != 0) {
						for (const uint64_t work_id : child.rpc_workers.complete(completed_work_id)) {
							start_rpc_work(child, work_id, global_tick_, module_ticks_[child.id.raw()]);
						}
					}
				}
			}
		}

		// Frames that become due at this tick are intentionally prepared after the next window's
		// inbound pages have been packed. As in strict mode, they become visible on the following
		// child handoff rather than racing the application at the same logical tick.
		platform_->prepare_tick(global_tick_);

		childsync::store_relaxed(control.done_count, 0);
		const uint32_t expected =
			static_cast<uint32_t>(std::ranges::count_if(children_, [](const Child& child) { return child.active; }));
		childsync::store_relaxed(control.expected, expected);

		// One wake for every child. The release pairs with each child's acquire on generation, so
		// the tick fields and inbound mailboxes above are visible before any of them runs.
		generation_++;
		childsync::store_release(control.generation, generation_);
		childsync::futex_wake(control.generation, INT_MAX);
	}

	void Runtime::collect_acknowledgements() {
		childsync::Control& control = control_->get();
		if (childsync::load_acquire(control.expected) == 0) {
			return;
		}

		// Waiting on the release word rather than on done_count costs one futex wait per tick: the
		// count changes once per child, and a futex wait returns on every change to its word.
		while (true) {
			const uint32_t released = childsync::load_acquire(control.release);
			if (released == generation_) {
				break;
			}

			if (!childsync::futex_wait(control.release, released, BARRIER_POLL)) {
				// Shared memory has no end of file, so a child that died is only visible by
				// reaping it. Checked on timeout alone, which a healthy tick never reaches.
				for (Child& child : children_) {
					if (child.process.exit_status().has_value()) {
						throw std::runtime_error("Lost the connection to module " + std::to_string(child.id.raw()));
					}
				}
			}
		}
	}

	void Runtime::retire_module(module::ModuleId id, uint64_t tick) {
		auto found = std::ranges::find_if(children_, [id](const Child& child) { return child.id == id; });
		if (found == children_.end() || !found->active) {
			return;
		}

		Child& child           = *found;
		child.active           = false;
		child.accepting_rpc    = false;
		child.tick_granted     = false;
		child.grant_mask       = 0;
		child.skipped_mask     = 0;
		child.retire_pending   = false;
		child.retire_after_ack = false;
		child.staged_outbound.clear();
		child.pending_requests.clear();
		child.pending_responses.clear();
		child.rpc_workers = platform::RpcWorkerPool{};
		child.rpc_work.clear();
		for (uint32_t index = 0; index < MAX_TICK_BATCH; index++) {
			childsync::mailbox_clear(child.segment->get().to_parent[index]);
			childsync::mailbox_clear(child.segment->get().to_child[index]);
		}
		control_->get().active_mask[id.raw()] = 0;
		child.process.terminate();

		if (trace_.enabled()) {
			trace_.write(trace::Entry("module_stopped")
							 .field("tick", tick)
							 .field("module", static_cast<uint64_t>(id.raw()))
							 .field("reason", platform_->stop_reason(id)));
		}
	}

	void Runtime::drain(Child& child, uint64_t source_global_tick, uint64_t source_local_tick, uint32_t batch_index) {
		childsync::Mailbox& mailbox = child.segment->get().to_parent[batch_index];
		if (!child.accepting_rpc) {
			childsync::mailbox_clear(mailbox);
			return;
		}

		// The child shares the page, so its offsets are checked before they are used to index it.
		if (!childsync::mailbox_valid(mailbox)) {
			throw std::runtime_error("Module " + std::to_string(child.id.raw()) + " corrupted its mailbox");
		}

		const std::size_t count = childsync::mailbox_count(mailbox);
		for (std::size_t index = 0; index < count; index++) {
			std::size_t                      length = 0;
			const uint8_t*                   bytes  = childsync::mailbox_read(mailbox, index, length);
			const platform::IngressAdmission ingress =
				platform_->account_ingress(child.id, platform::ModuleTime{source_global_tick, source_local_tick});
			rpc::Frame frame;
			reader_.clear();
			reader_.append(bytes, length);
			if (reader_.next(frame) != rpc::DecodeStatus::OK) {
				if (trace_.enabled()) {
					trace_.write(trace::Entry("rpc_ingress_drop")
									 .field("tick", source_global_tick)
									 .field("module", static_cast<uint64_t>(child.id.raw()))
									 .field("reason", "malformed_frame")
									 .field("length", static_cast<uint64_t>(length)));
				}
				if (ingress.stop_triggered) quiesce_module_for_stop(child, source_local_tick, std::nullopt);
				continue;
			}

			if (frame.type != MT_REQ) {
				if (trace_.enabled()) {
					trace_.write(trace::Entry("rpc_ingress_drop")
									 .field("tick", source_global_tick)
									 .field("module", static_cast<uint64_t>(child.id.raw()))
									 .field("reason", "unexpected_frame_type")
									 .field("frame_type", static_cast<uint64_t>(frame.type)));
				}
				continue;
			}

			uint64_t observation_id = 0;
			if (trace_.enabled() || !property_monitors_.empty()) {
				observation_id = next_rpc_observation_id_++;
				pending_rpc_requests_[source_global_tick].push_back(
					ltl::RpcRequest{observation_id, source_global_tick, child.id.raw(), frame.cmd, frame.call_id,
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
				quiesce_module_for_stop(child, source_local_tick, std::move(terminal));
				return;
			}

			// Requests become tasks so their order is decided by the seed, not by mailbox order.
			// The module and slot are fixed-width, so the prefix sorts numerically and is unique
			// within a tick: that is exactly the order this loop reads them in.
			const module::ModuleId id = child.id;
			char                   prefix[16];
			std::snprintf(prefix, sizeof(prefix), "rpc/%02u/%u/", id.raw(), static_cast<unsigned>(index));
			const std::string label = prefix + std::to_string(frame.call_id) + "/" + std::to_string(frame.cmd);

			child.pending_requests.push_back(PendingRpcRequest{source_local_tick + RPC_RX_DELAY_LOCAL_TICKS,
															   next_rpc_work_id_++, std::move(frame), std::move(label),
															   observation_id, ingress});
		}
	}

	void Runtime::release_local_rpc(Child& child, uint64_t global_tick, uint64_t local_tick) {
		while (!child.pending_responses.empty() && child.pending_responses.front().due_local_tick <= local_tick) {
			platform_->enqueue(child.id, std::move(child.pending_responses.front().frame));
			child.pending_responses.pop_front();
		}

		while (!child.pending_requests.empty() && child.pending_requests.front().due_local_tick <= local_tick) {
			PendingRpcRequest pending = std::move(child.pending_requests.front());
			child.pending_requests.pop_front();
			Child* const child_ptr = &child;
			std::string  label     = std::move(pending.label);
			queue_.schedule(
				global_tick, engine::Priority::CHILD_INTAKE, std::move(label),
				[this, child_ptr, local_tick, pending = std::move(pending)](engine::TaskContext& context) mutable {
					if (!child_ptr->accepting_rpc) return;
					const uint64_t         work_id = pending.work_id;
					const platform::Bucket bucket  = platform::request_bucket(pending.frame.cmd);
					child_ptr->rpc_work.emplace(work_id, std::move(pending));
					if (child_ptr->rpc_workers.enqueue(work_id, bucket).has_value()) {
						start_rpc_work(*child_ptr, work_id, context.tick(), local_tick);
					}
				});
		}
	}

	void Runtime::quiesce_module_for_stop(Child& child, uint64_t local_tick, std::optional<rpc::Frame> final_response) {
		child.accepting_rpc = false;
		child.pending_requests.clear();
		child.pending_responses.clear();
		child.staged_outbound.clear();
		child.rpc_workers = platform::RpcWorkerPool{};
		child.rpc_work.clear();
		for (uint32_t index = 0; index < MAX_TICK_BATCH; index++) {
			childsync::mailbox_clear(child.segment->get().to_child[index]);
		}

		const uint16_t module_id = child.id.raw();
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
			child.pending_responses.push_back(
				PendingRpcResponse{local_tick + RPC_TX_DELAY_LOCAL_TICKS, std::move(*final_response)});
		} else {
			child.retire_pending = true;
		}
	}

	void Runtime::start_rpc_work(Child& child, uint64_t work_id, uint64_t global_tick, uint64_t local_tick) {
		std::deque<uint64_t> work_to_start{work_id};
		while (!work_to_start.empty()) {
			const uint64_t current_work_id = work_to_start.front();
			work_to_start.pop_front();
			auto found = child.rpc_work.find(current_work_id);
			if (found == child.rpc_work.end()) {
				throw std::logic_error("RPC worker started an unknown request");
			}

			PendingRpcRequest pending = std::move(found->second);
			child.rpc_work.erase(found);
			std::optional<rpc::Frame> response = platform_->handle_request(
				child.id, pending.frame, platform::ModuleTime{global_tick, local_tick}, pending.ingress);
			if (!response) {
				if (platform_->lifecycle(child.id) == platform::Lifecycle::STOPPED) {
					quiesce_module_for_stop(child, local_tick, std::nullopt);
					return;
				}
				for (const uint64_t started : child.rpc_workers.complete(current_work_id)) {
					work_to_start.push_back(started);
				}
				continue;
			}

			rpc::Frame answer             = std::move(response.value());
			answer.observation_request_id = pending.observation_id;
			if (answer.stop_after_delivery) {
				quiesce_module_for_stop(child, local_tick, std::move(answer));
				return;
			}
			answer.handler_work_id = current_work_id;
			child.pending_responses.push_back(
				PendingRpcResponse{local_tick + RPC_TX_DELAY_LOCAL_TICKS, std::move(answer)});
		}
	}

	void Runtime::schedule_tick_tasks(uint32_t batch_index, uint32_t batch_ticks) {
		const uint64_t tick = global_tick_;

		queue_.schedule(tick, engine::Priority::WORLD_ADVANCE, "world",
						[this](engine::TaskContext&) { world_.advance_tick(); });

		queue_.schedule(tick, engine::Priority::TRACE_FINALIZE, "trace", [this](engine::TaskContext&) {
			if (current_observation_) trace_observation(*current_observation_);
		});

		queue_.schedule(tick, engine::Priority::MONITORING, "supervision", [this, tick](engine::TaskContext&) {
			for (Child& child : children_) {
				if (child.active && child.retire_pending) retire_module(child.id, tick);
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
				for (const Child& child : children_) {
					module_stepped.emplace(child.id.raw(), child.tick_granted);
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

		queue_.schedule(
			tick, engine::Priority::PLATFORM_OUTBOUND, "outbound",
			[this, tick, batch_index, batch_ticks](engine::TaskContext&) {
				platform_->advance(tick);
				for (Child& child : children_) {
					if (!child.active) continue;
					// A child has already executed the entire granted window. At its final global
					// boundary, stage what became ready during the replay; the next grant puts it in
					// the first slot that child will actually execute. If a whole window is skipped,
					// keep the already-observed delivery staged so `next` waits for its next real step.
					if (batch_index + 1U != batch_ticks || !child.staged_outbound.empty()) continue;
					for (rpc::Frame& frame : platform_->take_outbound(child.id, childsync::MAILBOX_CAPACITY)) {
						if (trace_.enabled() || !property_monitors_.empty()) {
							if (frame.type == MT_RESP && frame.observation_request_id != 0) {
								pending_rpc_responses_[tick].push_back(ltl::RpcResponse{
									frame.observation_request_id, frame.observation_request_id, tick, child.id.raw(),
									frame.cmd, frame.call_id, frame.status, frame.payload});
							} else if (frame.type == MT_EVENT) {
								pending_rpc_events_[tick].push_back(ltl::RpcEvent{
									next_rpc_observation_id_++, tick, child.id.raw(), frame.cmd, frame.payload});
							}
						}
						child.staged_outbound.push_back(std::move(frame));
					}
				}
			});
	}

	void Runtime::report_synchronization_stats() {
		if (!sync_stats_enabled_ || sync_stats_reported_) return;
		sync_stats_reported_ = true;

		const auto percentile = [](const auto& histogram, uint64_t samples, uint32_t percentage) {
			if (samples == 0) return std::size_t{0};
			const uint64_t target = (samples * percentage + 99U) / 100U;
			uint64_t       seen   = 0;
			for (std::size_t value = 0; value < histogram.size(); value++) {
				seen += histogram[value];
				if (seen >= target) return value;
			}
			return histogram.size() - 1;
		};
		const auto histogram_text = [](const auto& histogram) {
			std::ostringstream text;
			bool               first = true;
			for (std::size_t value = 0; value < histogram.size(); value++) {
				if (histogram[value] == 0) continue;
				if (!first) text << ',';
				first = false;
				text << value << ':' << histogram[value];
			}
			return text.str();
		};

		const double compression =
			sync_stats_.boundaries == 0 ? 0.0 : static_cast<double>(sync_stats_.logical_ticks) / sync_stats_.boundaries;
		const double amplification =
			sync_stats_.required_child_wakeups == 0
				? 0.0
				: static_cast<double>(sync_stats_.current_child_wakeups) / sync_stats_.required_child_wakeups;
		std::ostringstream report;
		report << std::fixed << std::setprecision(3);
		report << "synchronization performance:\n"
			   << "  logical_ticks=" << sync_stats_.logical_ticks << '\n'
			   << "  sync_boundaries=" << sync_stats_.boundaries << '\n'
			   << "  barrier_compression_ratio=" << compression << '\n'
			   << "  ticks_between_sync_p50=" << percentile(sync_stats_.ticks_per_boundary, sync_stats_.boundaries, 50)
			   << '\n'
			   << "  ticks_between_sync_p95=" << percentile(sync_stats_.ticks_per_boundary, sync_stats_.boundaries, 95)
			   << '\n'
			   << "  ticks_between_sync_p99=" << percentile(sync_stats_.ticks_per_boundary, sync_stats_.boundaries, 99)
			   << '\n'
			   << "  ticks_between_sync_max=" << percentile(sync_stats_.ticks_per_boundary, sync_stats_.boundaries, 100)
			   << '\n'
			   << "  ticks_between_sync_histogram=" << histogram_text(sync_stats_.ticks_per_boundary) << '\n'
			   << "  required_wake_set_p50="
			   << percentile(sync_stats_.required_wake_set_sizes, sync_stats_.boundaries, 50) << '\n'
			   << "  required_wake_set_p95="
			   << percentile(sync_stats_.required_wake_set_sizes, sync_stats_.boundaries, 95) << '\n'
			   << "  required_wake_set_p99="
			   << percentile(sync_stats_.required_wake_set_sizes, sync_stats_.boundaries, 99) << '\n'
			   << "  required_wake_set_max="
			   << percentile(sync_stats_.required_wake_set_sizes, sync_stats_.boundaries, 100) << '\n'
			   << "  required_wake_set_histogram=" << histogram_text(sync_stats_.required_wake_set_sizes) << '\n'
			   << "  current_child_wakeups=" << sync_stats_.current_child_wakeups << '\n'
			   << "  causally_required_child_wakeups=" << sync_stats_.required_child_wakeups << '\n'
			   << "  wake_amplification_ratio=" << amplification << '\n'
			   << "  boundary_limit_child_wake="
			   << sync_stats_.limits[static_cast<std::size_t>(BoundaryLimit::CHILD_WAKE)] << '\n'
			   << "  boundary_limit_engine_delivery="
			   << sync_stats_.limits[static_cast<std::size_t>(BoundaryLimit::ENGINE_DELIVERY)] << '\n'
			   << "  boundary_limit_hard_lookahead_cap="
			   << sync_stats_.limits[static_cast<std::size_t>(BoundaryLimit::HARD_LOOKAHEAD_CAP)] << '\n'
			   << "  boundary_limit_conservative_fallback="
			   << sync_stats_.limits[static_cast<std::size_t>(BoundaryLimit::CONSERVATIVE_FALLBACK)] << '\n';
		if (sync_stats_modules_) {
			report << "  per_module_wakes=";
			bool first = true;
			for (std::size_t id = 0; id < MODULE_TABLE_SIZE; id++) {
				if (sync_stats_.current_module_wakes[id] == 0 && sync_stats_.required_module_wakes[id] == 0) continue;
				if (!first) report << ',';
				first = false;
				report << id << ':' << sync_stats_.current_module_wakes[id] << '/'
					   << sync_stats_.required_module_wakes[id];
			}
			report << '\n';
		}
		std::cerr << report.str();
	}

	void Runtime::stop() {
		if (!running_ && children_.empty()) {
			return;
		}

		running_ = false;
		report_synchronization_stats();
		if (trace_.enabled()) {
			trace_.write(trace::Entry("run_complete").field("tick", global_tick_));
			trace_.flush();
		}
		for (Child& child : children_) {
			child.process.terminate();
		}
		children_.clear();

		// The mappings are anonymous, so unmapping them here and in the children is the whole
		// cleanup: there is no path left behind for a later run to trip over.
		control_.reset();
	}

} // namespace sim
