#include "platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "../module/module.h"
#include "../seed/seed.h"
#include "../settings.h"
#include "../world/world.h"

namespace platform {

	namespace {

		// The delivery model for logs and panic reports: the same loss and delay settings as ordinary
		// data packets, plus the extra hop to the hub Pi, under a separate profile name.
		net::Config log_channel_config() {
			net::Config config    = net::config_from_settings();
			config.extra_delay_ms = static_cast<uint64_t>(settings::get_setting<int>("network_log_extra_delay_ms"));
			config.profile += "/log";
			return config;
		}

		// A one-byte tag ahead of the payload says whether a diagnostic is a log or a panic report,
		// since both share one wire.
		constexpr uint8_t DIAGNOSTIC_KIND_LOG   = 0;
		constexpr uint8_t DIAGNOSTIC_KIND_PANIC = 1;

		std::vector<uint8_t> tag_diagnostic(uint8_t kind, const std::vector<uint8_t>& payload) {
			std::vector<uint8_t> tagged;
			tagged.reserve(1 + payload.size());
			tagged.push_back(kind);
			tagged.insert(tagged.end(), payload.begin(), payload.end());
			return tagged;
		}

		// Application logs arrive as already-formatted text. Prefix every physical output line at
		// the delivery boundary so multiline records retain the same shape as the platform console.
		void print_application_output(module::ModuleId id, const uint8_t* begin, const uint8_t* end) {
			std::string source = id.raw() == TOWER_MODULE_ID ? "[ATC]" : "[Pico " + std::to_string(id.raw()) + "]";
			source.resize(std::string_view{"[Pico 255]"}.size(), ' ');
			const std::string prefix = source + " [application] ";

			if (begin == end) {
				std::cout << prefix << '\n';
				std::cout.flush();
				return;
			}

			const uint8_t* line = begin;
			while (line != end) {
				const uint8_t* newline = std::find(line, end, static_cast<uint8_t>('\n'));
				std::cout << prefix;
				const uint8_t* line_end = newline == end ? end : newline + 1;
				std::cout.write(reinterpret_cast<const char*>(line), line_end - line);
				if (newline == end) {
					std::cout.put('\n');
				}
				line = line_end;
			}
			std::cout.flush();
		}

		// The commands the physical platform refuses on the tower, which owns no conveyor hardware.
		bool is_hardware_command(uint8_t cmd) {
			switch (cmd) {
			case CMD_LASER_LEFT_DETECT:
			case CMD_LASER_LEFT_GET:
			case CMD_LASER_LEFT_SET:
			case CMD_LASER_RIGHT_DETECT:
			case CMD_LASER_RIGHT_GET:
			case CMD_LASER_RIGHT_SET:
			case CMD_BELT_BIG_GET_ENCODER_COUNT:
			case CMD_BELT_BIG_GET_ENCODER_FREQ:
			case CMD_BELT_BIG_GET_SPEED:
			case CMD_BELT_BIG_SET_SPEED:
			case CMD_BELT_SMALL_GET_ENCODER_COUNT:
			case CMD_BELT_SMALL_GET_ENCODER_FREQ:
			case CMD_BELT_SMALL_GET_SPEED:
			case CMD_BELT_SMALL_SET_SPEED:
			case CMD_SERVO_ANGLE_GET:
			case CMD_SERVO_ANGLE_SET:
			case CMD_RFID_CHECK_TAG:
			case CMD_RFID_GET_UID:
			case CMD_RFID_READ_DATA_BLOCK:
			case CMD_RFID_WRITE_DATA_BLOCK:
			case CMD_CV_SENSOR_GET_CURRENT:
			case CMD_CV_SENSOR_GET_POWER:
			case CMD_CV_SENSOR_GET_VOLTAGE:        return true;
			default:                               return false;
			}
		}

		// Events an application may subscribe to. Encoder and current/voltage events are refused since
		// this version does not implement them.
		constexpr uint32_t SUPPORTED_EVENTS =
			static_cast<uint32_t>(EVENT_LASER_LEFT_DETECT) | static_cast<uint32_t>(EVENT_LASER_RIGHT_DETECT) |
			static_cast<uint32_t>(EVENT_RFID_DETECT) | static_cast<uint32_t>(EVENT_MESSAGE_RECEIVED);

		constexpr uint32_t HARDWARE_EVENTS = SUPPORTED_EVENTS & ~static_cast<uint32_t>(EVENT_MESSAGE_RECEIVED);

		constexpr std::size_t FLIGHT_BOARD_HEADER_SIZE = sizeof(quercus_flight_board_t);
		constexpr std::size_t FLIGHT_SIZE              = sizeof(quercus_flight_t);

		std::vector<uint8_t> byte_payload(uint8_t value) {
			return {value};
		}

		std::vector<uint8_t> visible_rfid_uid(const module::Module& module) {
			if (!module.is_rfid_active()) {
				return {};
			}
			const RfidUid& uid = module.rfid_uid();
			return {uid.begin(), uid.end()};
		}

		std::vector<uint8_t> float_payload(float value) {
			uint32_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			return {static_cast<uint8_t>(bits & 0xFF), static_cast<uint8_t>((bits >> 8) & 0xFF),
					static_cast<uint8_t>((bits >> 16) & 0xFF), static_cast<uint8_t>((bits >> 24) & 0xFF)};
		}

		// An encoder count is 4 bytes on the wire, matching the 32-bit `long` on the Pico and the
		// 32-bit Native Simulator. A native_sim//64 build reads 8 bytes and correctly rejects this.
		std::vector<uint8_t> int32_payload(int64_t value) {
			const uint32_t bits = static_cast<uint32_t>(static_cast<int32_t>(value));
			return {static_cast<uint8_t>(bits & 0xFF), static_cast<uint8_t>((bits >> 8) & 0xFF),
					static_cast<uint8_t>((bits >> 16) & 0xFF), static_cast<uint8_t>((bits >> 24) & 0xFF)};
		}

		std::vector<uint8_t> double_payload(double value) {
			uint64_t bits = 0;
			std::memcpy(&bits, &value, sizeof(bits));
			std::vector<uint8_t> payload(sizeof(bits));
			for (std::size_t index = 0; index < sizeof(bits); index++) {
				payload[index] = static_cast<uint8_t>((bits >> (8 * index)) & 0xFF);
			}
			return payload;
		}

		std::optional<float> parse_float(const std::vector<uint8_t>& payload) {
			if (payload.size() != 4) {
				return {};
			}
			float          value = 0;
			const uint32_t bits  = static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8) |
								  (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		}

		std::optional<uint32_t> parse_mask(const std::vector<uint8_t>& payload) {
			switch (payload.size()) {
			case 1: return static_cast<uint32_t>(payload[0]);
			case 2: return static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8);
			case 4:
				return static_cast<uint32_t>(payload[0]) | (static_cast<uint32_t>(payload[1]) << 8) |
					   (static_cast<uint32_t>(payload[2]) << 16) | (static_cast<uint32_t>(payload[3]) << 24);
			default: return {};
			}
		}

		// The flight board is variable length: a mirrored board is accepted only when its declared
		// flight counts match the payload exactly.
		bool is_valid_flight_board(const std::vector<uint8_t>& payload) {
			if (payload.size() < FLIGHT_BOARD_HEADER_SIZE) {
				return false;
			}
			const std::size_t flights = static_cast<std::size_t>(payload[2]) + static_cast<std::size_t>(payload[3]);
			return payload.size() == FLIGHT_BOARD_HEADER_SIZE + (flights * FLIGHT_SIZE);
		}

		// The bucket a command is charged to, alongside the global one.
		Bucket bucket_for(uint8_t cmd) {
			switch (cmd) {
			case CMD_LASER_LEFT_DETECT:
			case CMD_LASER_LEFT_GET:
			case CMD_LASER_LEFT_SET:
			case CMD_LASER_RIGHT_DETECT:
			case CMD_LASER_RIGHT_GET:
			case CMD_LASER_RIGHT_SET:        return Bucket::LASER;
			case CMD_BELT_BIG_GET_SPEED:
			case CMD_BELT_BIG_SET_SPEED:
			case CMD_BELT_SMALL_GET_SPEED:
			case CMD_BELT_SMALL_SET_SPEED:   return Bucket::BELT;
			case CMD_SERVO_ANGLE_GET:
			case CMD_SERVO_ANGLE_SET:        return Bucket::SERVO;
			case CMD_LED_GET_COLOR:
			case CMD_LED_SET_COLOR:          return Bucket::LED;
			case CMD_RFID_CHECK_TAG:
			case CMD_RFID_GET_UID:
			case CMD_RFID_READ_DATA_BLOCK:
			case CMD_RFID_WRITE_DATA_BLOCK:  return Bucket::RFID;
			case CMD_SEND_PACKET:
			case CMD_BROADCAST_PACKET:       return Bucket::NETWORK;
			case CMD_GET_OWN_ID:
			case CMD_GET_NETWORK_MAP:
			case CMD_GET_MODULE_LIST:        return Bucket::PLATFORM_MANAGEMENT;
			case CMD_SET_FLIGHT_SCHEDULE:    return Bucket::SCHEDULE;
			case CMD_SUBSCRIBE_TO_EVENT:
			case CMD_UNSUBSCRIBE_FROM_EVENT: return Bucket::EVENTS;
			case CMD_HEARTBEAT:              return Bucket::HEARTBEAT;
			case CMD_LOG:
			case CMD_PANIC_REPORT:           return Bucket::LOGGING;
			case CMD_CV_SENSOR_GET_CURRENT:
			case CMD_CV_SENSOR_GET_POWER:
			case CMD_CV_SENSOR_GET_VOLTAGE:  return Bucket::CV_SENSOR;
			default:                         return Bucket::GLOBAL;
			}
		}

		// A percentage of a capacity, rounded up.
		int threshold(int limit, int percent) {
			return ((limit * percent) + 99) / 100;
		}

		uint8_t neighbor_or_zero(const std::optional<module::ModuleId>& neighbor) {
			return neighbor ? neighbor->raw() : 0;
		}

		// Box-Muller over a dedicated deterministic seed stream. The simulator resolves hardware
		// event latency to whole millisecond ticks, so the final sample is rounded at the boundary.
		double standard_normal(seed::SplitMix64& stream) {
			const double scale  = 1.0 / 9007199254740992.0;
			const double first  = std::max(static_cast<double>(stream.next() >> 11) * scale, 1e-12);
			const double second = static_cast<double>(stream.next() >> 11) * scale;
			return std::sqrt(-2.0 * std::log(first)) * std::cos(2.0 * M_PI * second);
		}

	} // namespace

	std::string to_string(Bucket bucket) {
		switch (bucket) {
		case Bucket::GLOBAL:              return "global";
		case Bucket::SCHEDULE:            return "schedule";
		case Bucket::NETWORK:             return "network";
		case Bucket::LOGGING:             return "logging";
		case Bucket::RFID:                return "rfid";
		case Bucket::BELT:                return "belt";
		case Bucket::SERVO:               return "servo";
		case Bucket::PLATFORM_MANAGEMENT: return "platform_management";
		case Bucket::HEARTBEAT:           return "heartbeat";
		case Bucket::EVENTS:              return "events";
		case Bucket::LASER:               return "laser";
		case Bucket::LED:                 return "led";
		case Bucket::CV_SENSOR:           return "cv_sensor";
		case Bucket::COUNT:               break;
		}
		return "global";
	}

	int capacity_of(Bucket bucket) {
		// Requests per simulated second, aligned with the physical platform's buckets.
		switch (bucket) {
		case Bucket::GLOBAL:              return 100;
		case Bucket::SCHEDULE:            return 2;
		case Bucket::NETWORK:             return 5;
		case Bucket::LOGGING:             return 5;
		case Bucket::RFID:                return 10;
		case Bucket::BELT:                return 10;
		case Bucket::SERVO:               return 10;
		case Bucket::PLATFORM_MANAGEMENT: return 25;
		case Bucket::HEARTBEAT:           return 25;
		case Bucket::EVENTS:              return 25;
		case Bucket::LASER:               return 50;
		case Bucket::LED:                 return 50;
		case Bucket::CV_SENSOR:           return 10;
		case Bucket::COUNT:               break;
		}
		return 0;
	}

	Bucket request_bucket(uint8_t cmd) {
		return bucket_for(cmd);
	}

	Runtime* Manager::find(module::ModuleId id) {
		const auto found = runtimes_.find(id.raw());
		return found == runtimes_.end() ? nullptr : &found->second;
	}

	const Runtime* Manager::find(module::ModuleId id) const {
		const auto found = runtimes_.find(id.raw());
		return found == runtimes_.end() ? nullptr : &found->second;
	}

	Manager::Manager(world::State& world)
		: network_(static_cast<uint64_t>(settings::get_setting<int>("seed")), net::config_from_settings()),
		  log_network_(static_cast<uint64_t>(settings::get_setting<int>("seed")), log_channel_config()),
		  master_seed_(static_cast<uint64_t>(settings::get_setting<int>("seed"))),
		  laser_event_extra_delay_ms_(static_cast<uint64_t>(settings::get_setting<int>("laser_event_extra_delay_ms"))),
		  rfid_event_delay_mean_ms_(settings::get_setting<double>("rfid_event_extra_delay_mean_ms")),
		  rfid_event_delay_sd_ms_(settings::get_setting<double>("rfid_event_extra_delay_sd_ms")), world_(&world) {
		for (const module::ModuleId id : settings::get_configured_modules()) {
			known_modules_.insert(id.raw());
		}
		for (const module::ModuleId id : settings::get_active_modules()) {
			add_module(id);
		}
		rebuild_topology();
	}

	void Manager::add_module(module::ModuleId id) {
		known_modules_.insert(id.raw());
		if (find(id) != nullptr) {
			return;
		}
		Runtime runtime;
		runtime.present   = true;
		runtime.lifecycle = Lifecycle::ACTIVATING;
		runtimes_.emplace(id.raw(), std::move(runtime));
		modules_.push_back(id);
	}

	void Manager::rebuild_topology() {
		std::sort(modules_.begin(), modules_.end(),
				  [](module::ModuleId left, module::ModuleId right) { return left.raw() < right.raw(); });

		// The topology text applications parse: a rotation header, then one line per module.
		std::stringstream stream;
		stream << "0\n";
		for (const module::ModuleId id : modules_) {
			const settings::ModuleSettings module_settings = settings::get_module_settings(id);
			stream << "M" << static_cast<unsigned>(id.raw()) << ",t" << module_settings.type << ",a"
				   << static_cast<unsigned>(neighbor_or_zero(module_settings.left)) << ",b"
				   << static_cast<unsigned>(neighbor_or_zero(module_settings.middle)) << ",c"
				   << static_cast<unsigned>(neighbor_or_zero(module_settings.right)) << "\n";
		}
		network_map_ = stream.str();
	}

	bool Manager::charge(module::ModuleId id, uint8_t cmd, ModuleTime time) {
		Runtime& runtime = *find(id);
		bool     allowed = true;

		// Charged to its own bucket and the global one, so traffic cannot dodge the overall limit.
		for (const Bucket bucket : {Bucket::GLOBAL, request_bucket(cmd)}) {
			const std::size_t index = static_cast<std::size_t>(bucket);
			const int         limit = capacity_of(bucket);
			if (limit == 0) {
				continue;
			}

			std::deque<uint64_t>& samples = runtime.bucket_samples[index];
			samples.push_back(time.local_tick);
			const uint64_t oldest =
				time.local_tick >= (RATE_LIMIT_WINDOW_TICKS - 1) ? time.local_tick - (RATE_LIMIT_WINDOW_TICKS - 1) : 0;
			while (!samples.empty() && samples.front() < oldest) {
				samples.pop_front();
			}

			const int count = static_cast<int>(samples.size());

			// Escalation mirrors the platform: refuse, warn, pause, then stop.
			if (count >= threshold(limit, 120) &&
				(!runtime.bucket_has_warned[index] || runtime.bucket_warned_tick[index] != time.local_tick)) {
				runtime.bucket_has_warned[index]  = true;
				runtime.bucket_warned_tick[index] = time.local_tick;
				diagnostics_.push_back(Diagnostic{time.global_tick, id, bucket, "warning", count, limit});
			}
			if (count >= threshold(limit, 150) && runtime.paused_until_global_tick <= time.global_tick) {
				runtime.paused_until_global_tick = time.global_tick + PAUSE_DURATION_TICKS;
				runtime.lifecycle                = Lifecycle::PAUSED;
				diagnostics_.push_back(Diagnostic{time.global_tick, id, bucket, "pause", count, limit});
			}
			if (count >= threshold(limit, 200)) {
				runtime.lifecycle   = Lifecycle::STOPPED;
				runtime.stop_reason = to_string(bucket) + " bucket exceeded its stop threshold (" +
									  std::to_string(count) + "/" + std::to_string(limit) + ")";
				diagnostics_.push_back(Diagnostic{time.global_tick, id, bucket, "stop", count, limit});
				return false;
			}
			if (count >= limit) {
				allowed = false;
			}
		}

		return allowed;
	}

	void Manager::prepare_tick(uint64_t tick) {
		for (const module::ModuleId id : modules_) {
			Runtime& runtime = *find(id);
			if (runtime.lifecycle == Lifecycle::PAUSED && tick >= runtime.paused_until_global_tick) {
				runtime.lifecycle = runtime.heartbeat_seen ? Lifecycle::RUNNING : Lifecycle::ACTIVATING;
				diagnostics_.push_back(Diagnostic{tick, id, Bucket::GLOBAL, "resume", 0, 0});
			}
		}
	}

	void Manager::stop_application(uint64_t tick, module::ModuleId id, std::string reason) {
		Runtime* runtime_ptr = find(id);
		if (runtime_ptr == nullptr || runtime_ptr->lifecycle == Lifecycle::STOPPED) {
			return;
		}
		Runtime& runtime = *runtime_ptr;

		runtime.lifecycle                  = Lifecycle::STOPPED;
		runtime.stop_reason                = std::move(reason);
		runtime.subscriptions              = 0;
		runtime.heartbeat_seen             = false;
		runtime.last_heartbeat_global_tick = 0;
		runtime.heartbeat_warned           = false;
		runtime.heartbeat_failed           = true;
		runtime.paused_until_global_tick   = 0;
		runtime.bucket_samples             = {};
		runtime.bucket_warned_tick         = {};
		runtime.bucket_has_warned          = {};
		runtime.last_left_clear            = false;
		runtime.last_right_clear           = false;
		runtime.last_rfid_uid.clear();
		runtime.outbound.clear();

		std::erase_if(pending_hardware_events_, [id](const PendingHardwareEvent& event) { return event.module == id; });

		// Core0's safe stop affects this module's outputs only. Sensors and accumulated physical
		// state remain part of the world, while moving/energized actuators are made safe.
		if (module::Module* physical = world_->module(id)) {
			physical->set_big_belt_speed(module::BeltSpeed{});
			physical->set_small_belt_speed(module::BeltSpeed{});
			physical->set_servo_angle(physical->servo_actual_angle());
			physical->set_led_color(module::LedColor{});
			physical->set_left_laser_enabled(false);
			physical->set_right_laser_enabled(false);
		}

		diagnostics_.push_back(Diagnostic{tick, id, Bucket::HEARTBEAT, "stop", 0, 0});
	}

	std::vector<module::ModuleId> Manager::check_supervision(uint64_t tick) {
		for (const module::ModuleId id : modules_) {
			Runtime& runtime = *find(id);

			if (runtime.lifecycle == Lifecycle::STOPPED) {
				if (!runtime.heartbeat_failed) {
					throw std::runtime_error("Module " + std::to_string(id.raw()) +
											 " was stopped: " + runtime.stop_reason);
				}
				continue;
			}

			// A paused application gets no ticks, so it cannot be expected to do anything.
			if (runtime.lifecycle == Lifecycle::PAUSED) {
				continue;
			}

			if (!runtime.heartbeat_seen) {
				if (tick >= ACTIVATION_DEADLINE_TICKS) {
					stop_application(tick, id, "did not send a first heartbeat before its activation deadline");
				}
				continue;
			}

			const uint64_t silence = tick - runtime.last_heartbeat_global_tick;
			if (silence > HEARTBEAT_FAILURE_TICKS) {
				stop_application(tick, id, "stopped sending heartbeats");
				continue;
			}
			if (silence > HEARTBEAT_WARNING_TICKS && !runtime.heartbeat_warned) {
				runtime.heartbeat_warned = true;
				diagnostics_.push_back(Diagnostic{tick, id, Bucket::HEARTBEAT, "heartbeat_late", 0, 0});
			}
		}

		std::vector<module::ModuleId> stopped;
		for (const module::ModuleId id : modules_) {
			const Runtime* runtime = find(id);
			if (runtime->lifecycle == Lifecycle::STOPPED && runtime->heartbeat_failed) {
				stopped.push_back(id);
			}
		}
		return stopped;
	}

	std::vector<Diagnostic> Manager::take_diagnostics() {
		std::vector<Diagnostic> taken;
		taken.swap(diagnostics_);
		return taken;
	}

	std::optional<rpc::Frame> Manager::handle_request(module::ModuleId id, const rpc::Frame& request, ModuleTime time) {
		Runtime* runtime_ptr = find(id);
		if (runtime_ptr == nullptr) {
			return {};
		}
		Runtime& runtime = *runtime_ptr;
		if (runtime.lifecycle == Lifecycle::STOPPED) {
			return {};
		}

		// Recorded before dispatch, so a heartbeat counts even if its command is later refused.
		if (request.cmd == CMD_HEARTBEAT) {
			runtime.heartbeat_seen             = true;
			runtime.last_heartbeat_global_tick = time.global_tick;
			runtime.heartbeat_warned           = false;
			if (runtime.lifecycle == Lifecycle::ACTIVATING) {
				runtime.lifecycle = Lifecycle::RUNNING;
			}
		}

		if (!charge(id, request.cmd, time)) {
			// Over its limit: refuse rather than queue, matching what hardware would do.
			if ((request.flags & RPC_FLAG_NON_CON) != 0) {
				return {};
			}
			rpc::Frame response;
			response.type    = MT_RESP;
			response.cmd     = request.cmd;
			response.call_id = request.call_id;
			response.status  = STATUS_TRY_AGAIN;
			return response;
		}
		if (request.cmd == CMD_LOG) {
			log_network_.send(time.global_tick, id, {id}, tag_diagnostic(DIAGNOSTIC_KIND_LOG, request.payload));
		}
		if (request.cmd == CMD_PANIC_REPORT) {
			log_network_.send(time.global_tick, id, {id}, tag_diagnostic(DIAGNOSTIC_KIND_PANIC, request.payload));
		}

		if (request.cmd == CMD_SUBSCRIBE_TO_EVENT || request.cmd == CMD_UNSUBSCRIBE_FROM_EVENT) {
			rpc::Frame response;
			response.type    = MT_RESP;
			response.cmd     = request.cmd;
			response.call_id = request.call_id;

			const std::optional<uint32_t> mask = parse_mask(request.payload);
			if (!mask.has_value() || (mask.value() & ~SUPPORTED_EVENTS) != 0) {
				response.status = STATUS_INVALID;
			} else if (id == TOWER_MODULE_ID && (mask.value() & HARDWARE_EVENTS) != 0) {
				response.status = STATUS_PERM;
			} else if (request.cmd == CMD_SUBSCRIBE_TO_EVENT) {
				const uint32_t newly_subscribed = mask.value() & ~runtime.subscriptions;
				runtime.subscriptions |= mask.value();
				if ((newly_subscribed & EVENT_RFID_DETECT) != 0) {
					const module::Module* module = world_->module(id);
					runtime.last_rfid_uid = module == nullptr ? std::vector<uint8_t>{} : visible_rfid_uid(*module);
				}
			} else {
				runtime.subscriptions &= ~mask.value();
			}

			if ((request.flags & RPC_FLAG_NON_CON) != 0) {
				return {};
			}
			return response;
		}

		if (request.cmd == CMD_SET_FLIGHT_SCHEDULE) {
			rpc::Frame response;
			response.type    = MT_RESP;
			response.cmd     = request.cmd;
			response.call_id = request.call_id;

			if (!(id == TOWER_MODULE_ID)) {
				response.status = STATUS_PERM;
			} else if (!is_valid_flight_board(request.payload)) {
				response.status = STATUS_INVALID;
			} else {
				flight_schedule_ = request.payload;
			}

			if ((request.flags & RPC_FLAG_NON_CON) != 0) {
				return {};
			}
			return response;
		}

		rpc::Frame response = dispatch(id, request, time.global_tick);
		if ((request.flags & RPC_FLAG_NON_CON) != 0) {
			return {};
		}
		return response;
	}

	void Manager::publish(module::ModuleId id, uint32_t event, std::vector<uint8_t> payload) {
		Runtime* runtime = find(id);
		if (runtime == nullptr || runtime->lifecycle == Lifecycle::STOPPED || (runtime->subscriptions & event) == 0) {
			return;
		}

		rpc::Frame frame;
		frame.type    = MT_EVENT;
		frame.cmd     = static_cast<uint8_t>(event);
		frame.payload = std::move(payload);
		runtime->outbound.push_back(std::move(frame));
	}

	void Manager::schedule_hardware_event(uint64_t tick, module::ModuleId id, uint32_t event,
										  std::vector<uint8_t> payload, uint64_t delay_ms) {
		Runtime* runtime = find(id);
		if (runtime == nullptr || runtime->lifecycle == Lifecycle::STOPPED || (runtime->subscriptions & event) == 0) {
			return;
		}

		rpc::Frame frame;
		frame.type    = MT_EVENT;
		frame.cmd     = static_cast<uint8_t>(event);
		frame.payload = std::move(payload);
		pending_hardware_events_.push_back(
			PendingHardwareEvent{tick + delay_ms, next_hardware_event_ordinal_++, id, std::move(frame)});
	}

	uint64_t Manager::sample_rfid_event_delay_ms(module::ModuleId id) {
		const auto       key  = std::make_pair(id.raw(), static_cast<uint32_t>(EVENT_RFID_DETECT));
		const uint64_t   draw = hardware_event_draw_ordinals_[key]++;
		seed::SplitMix64 stream(seed::derive(master_seed_, "hardware-event-delay/" + std::to_string(id.raw()) + "/" +
															   std::to_string(EVENT_RFID_DETECT) + "/" +
															   std::to_string(draw)));
		const double     sampled = rfid_event_delay_mean_ms_ + rfid_event_delay_sd_ms_ * standard_normal(stream);
		return static_cast<uint64_t>(std::max<int64_t>(0, std::llround(sampled)));
	}

	void Manager::deliver_hardware_events(uint64_t tick) {
		std::stable_sort(pending_hardware_events_.begin(), pending_hardware_events_.end(),
						 [](const PendingHardwareEvent& left, const PendingHardwareEvent& right) {
							 return std::tie(left.due_tick, left.ordinal) < std::tie(right.due_tick, right.ordinal);
						 });

		auto first_pending = pending_hardware_events_.begin();
		while (first_pending != pending_hardware_events_.end() && first_pending->due_tick <= tick) {
			if (Runtime* runtime = find(first_pending->module);
				runtime != nullptr && runtime->lifecycle != Lifecycle::STOPPED) {
				runtime->outbound.push_back(std::move(first_pending->frame));
			}
			first_pending++;
		}
		pending_hardware_events_.erase(pending_hardware_events_.begin(), first_pending);
	}

	bool Manager::enqueue_packet(uint64_t tick, module::ModuleId sender, const rpc::Frame& request, bool broadcast) {
		const Runtime* sender_runtime = find(sender);
		if (sender_runtime == nullptr || sender_runtime->lifecycle == Lifecycle::STOPPED) {
			return false;
		}

		std::vector<uint8_t>          data;
		std::vector<module::ModuleId> recipients;

		if (broadcast) {
			// Broadcast to all modules, except for back to the
			data       = request.payload;
			recipients = modules_ | std::ranges::views::filter([sender](module::ModuleId id) { return id != sender; }) |
						 std::ranges::to<std::vector>();
		} else {
			if (request.payload.empty()) {
				return false;
			}
			const uint8_t destination = request.payload[0];
			if (!known_modules_.contains(destination)) {
				return false;
			}
			data.assign(request.payload.begin() + 1, request.payload.end());
			recipients.push_back(module::ModuleId{destination});
		}

		network_.send(tick, sender, recipients, data);
		return true;
	}

	void Manager::advance(uint64_t tick) {
		for (const module::ModuleId id : modules_) {
			Runtime& runtime = runtimes_[id.raw()];
			if (runtime.lifecycle == Lifecycle::STOPPED) {
				continue;
			}
			const module::Module* module = world_->module(id);
			if (module == nullptr) {
				continue;
			}

			// Sensor events report a change, not a level, so publish only on a difference.
			const bool left_clear  = module->is_laser_clear(true);
			const bool right_clear = module->is_laser_clear(false);

			if (!left_clear && runtime.last_left_clear)
				schedule_hardware_event(tick, id, EVENT_LASER_LEFT_DETECT, {}, laser_event_extra_delay_ms_);
			if (!right_clear && runtime.last_right_clear)
				schedule_hardware_event(tick, id, EVENT_LASER_RIGHT_DETECT, {}, laser_event_extra_delay_ms_);

			runtime.last_left_clear  = left_clear;
			runtime.last_right_clear = right_clear;

			const std::vector<uint8_t> uid_payload = visible_rfid_uid(*module);
			if (uid_payload != runtime.last_rfid_uid) {
				runtime.last_rfid_uid = uid_payload;
				// Only an arriving tag is worth an event; a departing one is observed by polling.
				if (!uid_payload.empty()) {
					schedule_hardware_event(tick, id, EVENT_RFID_DETECT, uid_payload, sample_rfid_event_delay_ms(id));
				}
			}
		}

		deliver_hardware_events(tick);

		for (net::Delivery& delivery : network_.deliver_due(tick)) {
			publish(delivery.recipient, EVENT_MESSAGE_RECEIVED, std::move(delivery.payload));
		}

		for (net::Delivery& delivery : log_network_.deliver_due(tick)) {
			Runtime* runtime = find(delivery.recipient);
			if (runtime == nullptr) {
				continue;
			}
			const uint8_t kind = delivery.payload.front();
			if (kind == DIAGNOSTIC_KIND_PANIC) {
				runtime->panics.emplace_back(delivery.payload.begin() + 1, delivery.payload.end());
			} else {
				runtime->logs.emplace_back(delivery.payload.begin() + 1, delivery.payload.end());
			}
			print_application_output(delivery.recipient, delivery.payload.data() + 1,
									 delivery.payload.data() + delivery.payload.size());
		}
	}

	void Manager::enqueue(module::ModuleId id, rpc::Frame frame) {
		Runtime* runtime = find(id);
		if (runtime == nullptr || runtime->lifecycle == Lifecycle::STOPPED) {
			return;
		}
		runtime->outbound.push_back(std::move(frame));
	}

	std::vector<rpc::Frame> Manager::take_outbound(module::ModuleId id, std::size_t byte_budget) {
		std::vector<rpc::Frame> frames;
		Runtime*                runtime = find(id);
		if (runtime == nullptr || runtime->lifecycle == Lifecycle::STOPPED) {
			return frames;
		}

		while (!runtime->outbound.empty() && frames.size() < MAX_OUTBOUND_PER_TICK) {
			const std::size_t size = rpc::HEADER_SIZE + runtime->outbound.front().payload.size();
			if (size > byte_budget) {
				break;
			}
			byte_budget -= size;
			frames.push_back(std::move(runtime->outbound.front()));
			runtime->outbound.pop_front();
		}

		return frames;
	}

	std::size_t Manager::backlog(module::ModuleId id) const {
		const Runtime* runtime = find(id);
		return runtime == nullptr ? 0 : runtime->outbound.size();
	}

	rpc::Frame Manager::dispatch(module::ModuleId id, const rpc::Frame& request, uint64_t tick) {
		rpc::Frame response;
		response.type    = MT_RESP;
		response.cmd     = request.cmd;
		response.call_id = request.call_id;
		response.status  = STATUS_OK;

		if (id == TOWER_MODULE_ID && is_hardware_command(request.cmd)) {
			response.status = STATUS_PERM;
			return response;
		}

		module::Module* module = world_->module(id);
		if (module == nullptr) {
			response.status = STATUS_NO_ENTRY;
			return response;
		}

		switch (request.cmd) {
		case CMD_LASER_LEFT_DETECT:  response.payload = byte_payload(module->is_laser_clear(true) ? 1 : 0); break;
		case CMD_LASER_RIGHT_DETECT: response.payload = byte_payload(module->is_laser_clear(false) ? 1 : 0); break;
		case CMD_LASER_LEFT_GET:     response.payload = byte_payload(module->is_left_laser_enabled() ? 1 : 0); break;
		case CMD_LASER_RIGHT_GET:    response.payload = byte_payload(module->is_right_laser_enabled() ? 1 : 0); break;
		case CMD_LASER_LEFT_SET:
			if (request.payload.size() != 1) {
				response.status = STATUS_INVALID;
				break;
			}
			module->set_left_laser_enabled(request.payload[0] != 0);
			break;
		case CMD_LASER_RIGHT_SET:
			if (request.payload.size() != 1) {
				response.status = STATUS_INVALID;
				break;
			}
			module->set_right_laser_enabled(request.payload[0] != 0);
			break;

		case CMD_BELT_BIG_GET_SPEED:
			response.payload = float_payload(static_cast<float>(module->big_belt_speed().raw()));
			break;
		case CMD_BELT_SMALL_GET_SPEED:
			response.payload = float_payload(static_cast<float>(module->small_belt_speed().raw()));
			break;

		case CMD_BELT_BIG_GET_ENCODER_COUNT:
		case CMD_BELT_SMALL_GET_ENCODER_COUNT: {
			const world::Calibration& calibration = world::calibration_for(id);
			const int64_t travel = request.cmd == CMD_BELT_BIG_GET_ENCODER_COUNT ? module->big_belt_travel_um()
																				 : module->small_belt_travel_um();
			response.payload     = int32_payload(world::encoder_count(travel, calibration));
			break;
		}
		case CMD_BELT_BIG_GET_ENCODER_FREQ:
		case CMD_BELT_SMALL_GET_ENCODER_FREQ: {
			const world::Calibration& calibration = world::calibration_for(id);
			const bool                big         = request.cmd == CMD_BELT_BIG_GET_ENCODER_FREQ;
			const double              speed = big ? module->big_belt_speed().raw() : module->small_belt_speed().raw();
			const world::BeltCurve&   curve = big ? calibration.mechanical.big_belt : calibration.mechanical.small_belt;
			response.payload                = double_payload(world::encoder_frequency_hz(speed, curve, calibration));
			break;
		}

		case CMD_CV_SENSOR_GET_CURRENT:
		case CMD_CV_SENSOR_GET_POWER:
		case CMD_CV_SENSOR_GET_VOLTAGE: {
			const world::Electrics electrics = world::electrics_of(*module, world::calibration_for(id));
			const double           value     = request.cmd == CMD_CV_SENSOR_GET_CURRENT ? electrics.current_a
											   : request.cmd == CMD_CV_SENSOR_GET_POWER ? electrics.power_w
																						: electrics.voltage_v;
			response.payload                 = float_payload(static_cast<float>(value));
			break;
		}
		case CMD_BELT_BIG_SET_SPEED:
		case CMD_BELT_SMALL_SET_SPEED: {
			const std::optional<float> speed = parse_float(request.payload);
			// A belt runs stopped or between a quarter and full speed. Anything else is refused,
			// since the physical platform cannot produce it either.
			if (!speed.has_value() || !world::is_valid_belt_command_percent(static_cast<double>(speed.value()))) {
				response.status = STATUS_INVALID;
				break;
			}
			const module::BeltSpeed value{static_cast<double>(speed.value())};
			if (request.cmd == CMD_BELT_BIG_SET_SPEED) {
				module->set_big_belt_speed(value);
			} else {
				module->set_small_belt_speed(value);
			}
			break;
		}

		case CMD_SERVO_ANGLE_GET:
			response.payload = float_payload(static_cast<float>(module->servo_angle().raw()));
			break;
		case CMD_SERVO_ANGLE_SET: {
			const std::optional<float> angle = parse_float(request.payload);
			if (!angle.has_value() || !std::isfinite(angle.value()) || angle.value() < 0.0F || angle.value() > 180.0F) {
				response.status = STATUS_INVALID;
				break;
			}
			// A servo has no feedback, so the reported angle is always the commanded one.
			world::command_servo(*module, module::ServoAngle{static_cast<double>(angle.value())});
			break;
		}

		case CMD_LED_GET_COLOR: {
			const uint32_t color = module->led_color().raw();
			response.payload     = {static_cast<uint8_t>(color & 0xFF), static_cast<uint8_t>((color >> 8) & 0xFF),
									static_cast<uint8_t>((color >> 16) & 0xFF), static_cast<uint8_t>((color >> 24) & 0xFF)};
			break;
		}
		case CMD_LED_SET_COLOR: {
			if (request.payload.size() != 4) {
				response.status = STATUS_INVALID;
				break;
			}
			const uint32_t color =
				static_cast<uint32_t>(request.payload[0]) | (static_cast<uint32_t>(request.payload[1]) << 8) |
				(static_cast<uint32_t>(request.payload[2]) << 16) | (static_cast<uint32_t>(request.payload[3]) << 24);
			module->set_led_color(module::LedColor{color});
			break;
		}

		case CMD_RFID_CHECK_TAG: response.payload = byte_payload(module->is_rfid_active() ? 1 : 0); break;
		case CMD_RFID_GET_UID:
			// A zero-length UID means no tag; this is not an error.
			if (module->is_rfid_active()) {
				const RfidUid& uid = module->rfid_uid();
				response.payload.assign(uid.begin(), uid.end());
			}
			break;
		case CMD_RFID_READ_DATA_BLOCK: {
			if (request.payload.size() != 1) {
				response.status = STATUS_INVALID;
				break;
			}
			const std::size_t page = request.payload[0];
			if (page >= RFID_PAYLOAD_PAGE_COUNT) {
				response.status = STATUS_RANGE;
				break;
			}
			if (!module->is_rfid_active()) {
				response.status = STATUS_NO_ENTRY;
				break;
			}
			const RfidPage value = module->rfid_page(page);
			response.payload.assign(value.begin(), value.end());
			break;
		}
		case CMD_RFID_WRITE_DATA_BLOCK: {
			if (request.payload.size() != 1 + RFID_PAGE_SIZE_BYTES) {
				response.status = STATUS_INVALID;
				break;
			}
			if (request.payload[0] >= RFID_PAYLOAD_PAGE_COUNT) {
				response.status = STATUS_RANGE;
				break;
			}
			if (!module->is_rfid_active()) {
				response.status = STATUS_NO_ENTRY;
				break;
			}
			RfidPage value{};
			std::copy(request.payload.begin() + 1, request.payload.end(), value.begin());
			if (!module->write_rfid_page(request.payload[0], value)) {
				response.status = STATUS_INVALID;
			} else {
				// The module buffer models the reader and is repopulated from the physical tub every
				// world tick. Persist the successful write in that authoritative world state too.
				(void)world_->write_rfid_page(id, request.payload[0], value);
			}
			break;
		}

		case CMD_SEND_PACKET:
		case CMD_BROADCAST_PACKET:
			if (!enqueue_packet(tick, id, request, request.cmd == CMD_BROADCAST_PACKET)) {
				response.status = STATUS_NO_ENTRY;
			}
			break;

		case CMD_GET_OWN_ID:      response.payload = byte_payload(id.raw()); break;
		case CMD_GET_NETWORK_MAP: response.payload.assign(network_map_.begin(), network_map_.end()); break;
		case CMD_GET_MODULE_LIST: {
			response.payload.reserve(2 + (5 * modules_.size()));
			response.payload.push_back(static_cast<uint8_t>(modules_.size() & 0xFF));
			response.payload.push_back(static_cast<uint8_t>((modules_.size() >> 8) & 0xFF));
			for (const module::ModuleId listed : modules_) {
				response.payload.push_back(listed.raw());
				response.payload.push_back(10);
				response.payload.push_back(0);
				response.payload.push_back(0);
				response.payload.push_back(listed.raw());
			}
			break;
		}

		case CMD_HEARTBEAT:
		case CMD_LOG:
		case CMD_PANIC_REPORT:
			// Already recorded before dispatch.
			break;

		default: response.status = STATUS_NO_SYS; break;
		}

		return response;
	}

	uint32_t Manager::subscriptions(module::ModuleId id) const {
		const Runtime* runtime = find(id);
		return runtime == nullptr ? 0 : runtime->subscriptions;
	}

	Lifecycle Manager::lifecycle(module::ModuleId id) const {
		const Runtime* runtime = find(id);
		return runtime == nullptr ? Lifecycle::STOPPED : runtime->lifecycle;
	}

	const std::string& Manager::stop_reason(module::ModuleId id) const {
		static const std::string none;
		const Runtime*           runtime = find(id);
		return runtime == nullptr ? none : runtime->stop_reason;
	}

	bool Manager::is_paused(module::ModuleId id, uint64_t tick) const {
		const Runtime* runtime = find(id);
		return runtime != nullptr && runtime->lifecycle == Lifecycle::PAUSED &&
			   tick < runtime->paused_until_global_tick;
	}

	bool Manager::is_activated(module::ModuleId id) const {
		const Runtime* runtime = find(id);
		return runtime != nullptr && runtime->heartbeat_seen;
	}

	const std::vector<std::string>& Manager::logs(module::ModuleId id) const {
		static const std::vector<std::string> none;
		const Runtime*                        runtime = find(id);
		return runtime == nullptr ? none : runtime->logs;
	}

	const std::vector<std::string>& Manager::panics(module::ModuleId id) const {
		static const std::vector<std::string> none;
		const Runtime*                        runtime = find(id);
		return runtime == nullptr ? none : runtime->panics;
	}

} // namespace platform
