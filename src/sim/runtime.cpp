#include "runtime.h"

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

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

	Runtime::Runtime(uint64_t seed, std::chrono::milliseconds connect_timeout,
					 std::chrono::milliseconds activation_timeout, uint32_t tick_batch)
		: seed_(seed), connect_timeout_(connect_timeout), activation_timeout_(activation_timeout),
		  tick_batch_(tick_batch), active_modules_(settings::get_active_modules()), queue_(seed) {
		if (active_modules_.empty()) throw std::runtime_error("No active modules in layout.toml");
		for (const module::ModuleId id : active_modules_) world_.add_module(id);
		property_observer_.emplace(active_modules_, world_);
	}

	Runtime::~Runtime() {
		stop();
	}

	void Runtime::start(std::optional<uint64_t> last_tick) {
		if (tick_batch_ == 0 || tick_batch_ > MAX_TICK_BATCH) {
			throw std::invalid_argument("Tick batch must be between 1 and " + std::to_string(MAX_TICK_BATCH));
		}

		control_.emplace();
		childsync::Control& control = control_->get();

		// Published before any application launches, so a stale binary fails at startup with a
		// clear message rather than silently reading the segment at the wrong offsets.
		childsync::store_release(control.abi, QUERCUS_SHM_ABI);

		// All children participate initially. Heartbeat supervision can reduce this before a later
		// generation after the failed child has been reaped.
		childsync::store_relaxed(control.expected, static_cast<uint32_t>(active_modules_.size()));
		control_->mapping().keep_across_exec();

		for (const module::ModuleId id : active_modules_) {
			Child child;
			child.id = id;
			seed::SplitMix64 timing(seed::derive(seed_, "module-clock/" + std::to_string(id.raw())));
			const double     start_ms =
				settings::get_setting<double>("module_start_offset_mean_ms") +
				settings::get_setting<double>("module_start_offset_sd_ms") * standard_normal(timing);
			child.start_tick     = start_ms <= 0.0 ? 0u : static_cast<uint64_t>(std::llround(start_ms));
			const double drift   = settings::get_setting<double>("module_clock_drift_sd_ppm") * standard_normal(timing);
			child.clock_rate_ppm = std::max<int64_t>(1, 1'000'000 + static_cast<int64_t>(std::llround(drift)));
			const double skip_ppm = settings::get_setting<double>("module_tick_skip_mean_ppm") +
									settings::get_setting<double>("module_tick_skip_sd_ppm") * standard_normal(timing);
			child.tick_skip_ppm = static_cast<uint32_t>(std::clamp(std::llround(skip_ppm), 0LL, 1'000'000LL));
			child.tick_skip_rng = seed::derive(seed_, "module-clock-skips/" + std::to_string(id.raw()));
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
			step(1);
			if (failed() || (!property_monitors_.empty() && properties_pending() == 0)) break;
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
			const uint64_t until_tick = declaration.scope ? declaration.scope->until_tick : scenario_duration_tick_;
			trace_.write(trace::Entry("property_activation")
							 .field("name", declaration.name)
							 .field("source", property_filename_)
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
			const std::string current = std::to_string(tub.module) + "/" + std::to_string(static_cast<int>(tub.belt)) +
										"/" + std::to_string(tub.position_um);
			std::string& previous = last_world_[id];
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
			queue_.schedule(*tub.removed_tick, engine::Priority::SCENARIO_EVENTS,
							"tub/" + std::to_string(id) + "/removed",
							[this, id](engine::TaskContext&) { world_.remove(id); });
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
		}
		scenario_duration_tick_ = duration_tick;
		property_filename_      = filename;
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
			results.push_back(PropertyResult{declaration.name, property_filename_, declaration.span.begin, verdict,
											 property_decisive_ticks_[index], monitor.failure(), monitor.activations(),
											 monitor.has_trigger(), monitor.obligation_at_end()});
		}
		return results;
	}

	void Runtime::advance(std::chrono::microseconds elapsed, double speed, std::chrono::microseconds deadline) {
		if (!running_ || speed <= 0.0 || failed()) {
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
			const uint32_t batch    = tick_batch_;
			const uint32_t advanced = step(batch);
			index += advanced;

			// A decisive property result ends the run inside the tick that produced it, state intact.
			if (failed() || (!property_monitors_.empty() && properties_pending() == 0)) {
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

	uint32_t Runtime::step(uint32_t batch_ticks) {
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
			schedule_tick_tasks(batch_index);
			queue_.run_tick(global_tick_);
			global_tick_++;
			completed++;
			if (failed() || (!property_monitors_.empty() && properties_pending() == 0)) {
				break;
			}
		}
		return completed;
	}

	void Runtime::grant_tick(uint32_t batch_ticks) {
		childsync::Control& control = control_->get();

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

	void Runtime::schedule_tick_tasks(uint32_t batch_index) {
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
				for (std::size_t index = 0; index < property_monitors_.size(); index++) {
					if (tick > scenario_duration_tick_) continue;
					const ltl::PropertyDeclaration& declaration = properties_->properties[index];
					const uint64_t                  from_tick   = declaration.scope ? declaration.scope->from_tick : 0;
					const uint64_t                  until_tick =
                        declaration.scope ? declaration.scope->until_tick : scenario_duration_tick_;
					if (tick < from_tick || tick > until_tick) continue;

					if (property_decisive_ticks_[index]) continue;

					ltl::Monitor& monitor = property_monitors_[index];
					if (monitor.verdict() == ltl::Verdict::PENDING) {
						const auto actual = [&observation](const ltl::Formula& atom) {
							return observation.actual_value(atom);
						};
						(void)monitor.observe(
							[&observation](const ltl::Formula& atom) { return observation.evaluate(atom); }, actual,
							[&observation](const ltl::Value* domain, ltl::ValueType type,
										   const ltl::DomainVisitor& visit) {
								observation.visit_domain(domain, type, visit);
							},
							[&observation](uint16_t module) { return observation.stepped(module); });
						if (tick == until_tick) (void)monitor.finish(actual);
					}
					if (monitor.verdict() != ltl::Verdict::PENDING) {
						property_decisive_ticks_[index] = tick;
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
									entry.field("obligation_created_tick", *monitor.failure()->obligation_created_tick);
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
		});

		queue_.schedule(
			tick, engine::Priority::PLATFORM_OUTBOUND, "outbound", [this, tick, batch_index](engine::TaskContext&) {
				platform_->advance(tick);
				for (Child& child : children_) {
					if (!child.active) continue;
					// A child has already executed the entire granted window. At its final global
					// boundary, stage what became ready during the replay; the next grant puts it in
					// the first slot that child will actually execute. If a whole window is skipped,
					// keep the already-observed delivery staged so `next` waits for its next real step.
					if (batch_index + 1U != tick_batch_ || !child.staged_outbound.empty()) continue;
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

	void Runtime::stop() {
		if (!running_ && children_.empty()) {
			return;
		}

		running_ = false;
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
