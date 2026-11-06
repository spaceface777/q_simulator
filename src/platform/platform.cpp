#include "platform.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <ranges>
#include <span>
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
			std::vector<uint8_t> tagged(1 + payload.size());
			tagged.front() = kind;
			std::copy(payload.begin(), payload.end(), tagged.begin() + 1);
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
			case CMD_RFID_READ_DATA_FULL:
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

		std::span<const uint8_t> visible_rfid_uid(const module::Module& module) {
			if (!module.is_rfid_active()) {
				return {};
			}
			return module.rfid_uid();
		}

		template <typename T> std::vector<uint8_t> native_payload(T value) {
			std::vector<uint8_t> payload(sizeof(value));
			std::memcpy(payload.data(), &value, sizeof(value));
			return payload;
		}

		template <typename T> std::optional<T> parse_native(const std::vector<uint8_t>& payload) {
			if (payload.size() != sizeof(T)) {
				return {};
			}
			T value{};
			std::memcpy(&value, payload.data(), sizeof(value));
			return value;
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
			case CMD_LASER_RIGHT_SET:              return Bucket::LASER;
			case CMD_BELT_BIG_GET_ENCODER_COUNT:
			case CMD_BELT_BIG_GET_ENCODER_FREQ:
			case CMD_BELT_BIG_GET_SPEED:
			case CMD_BELT_BIG_SET_SPEED:
			case CMD_BELT_SMALL_GET_ENCODER_COUNT:
			case CMD_BELT_SMALL_GET_ENCODER_FREQ:
			case CMD_BELT_SMALL_GET_SPEED:
			case CMD_BELT_SMALL_SET_SPEED:         return Bucket::BELT;
			case CMD_SERVO_ANGLE_GET:
			case CMD_SERVO_ANGLE_SET:              return Bucket::SERVO;
			case CMD_LED_GET_COLOR:
			case CMD_LED_SET_COLOR:                return Bucket::LED;
			case CMD_RFID_CHECK_TAG:
			case CMD_RFID_GET_UID:
			case CMD_RFID_READ_DATA_BLOCK:
			case CMD_RFID_READ_DATA_FULL:
			case CMD_RFID_WRITE_DATA_BLOCK:        return Bucket::RFID;
			case CMD_SEND_PACKET:
			case CMD_BROADCAST_PACKET:             return Bucket::NETWORK;
			case CMD_GET_OWN_ID:
			case CMD_GET_NETWORK_MAP:
			case CMD_GET_MODULE_LIST:              return Bucket::PLATFORM_MANAGEMENT;
			case CMD_SET_FLIGHT_SCHEDULE:          return Bucket::SCHEDULE;
			case CMD_SUBSCRIBE_TO_EVENT:
			case CMD_UNSUBSCRIBE_FROM_EVENT:       return Bucket::EVENTS;
			case CMD_HEARTBEAT:                    return Bucket::HEARTBEAT;
			case CMD_LOG:
			case CMD_PANIC_REPORT:                 return Bucket::LOGGING;
			case CMD_CV_SENSOR_GET_CURRENT:
			case CMD_CV_SENSOR_GET_POWER:
			case CMD_CV_SENSOR_GET_VOLTAGE:        return Bucket::CV_SENSOR;
			case CMD_ENTROPY_GET:                  return Bucket::ENTROPY;
			case CMD_ATTEST:
			case CMD_ATTEST_VERIFY:                return Bucket::ATTESTATION;
			default:                               return Bucket::GLOBAL;
			}
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

		// Closes the current window once it is a second old; a new one starts with the next request.
		void refresh_window(RateState& state, uint64_t tick) {
			if (state.current.count == 0) {
				state.current.start_tick = tick;
				return;
			}
			if (tick - state.current.start_tick < RATE_LIMIT_WINDOW_TICKS) return;
			state.history[state.history_head] = state.current;
			state.history_head                = static_cast<uint8_t>((state.history_head + 1) % state.history.size());
			state.current                     = RateWindow{tick, 0};
		}

		void record_request(RateState& state, uint64_t tick, int cost) {
			refresh_window(state, tick);
			state.current.count = static_cast<uint16_t>(std::min<int>(state.current.count + cost, UINT16_MAX));
		}

		// The busiest window that started within the last five seconds.
		int peak_count(const RateState& state, uint64_t tick) {
			int peak = 0;
			if (state.current.count > 0 && tick - state.current.start_tick < RATE_LIMIT_HISTORY_TICKS)
				peak = state.current.count;
			for (const RateWindow& window : state.history) {
				if (window.count > 0 && tick - window.start_tick < RATE_LIMIT_HISTORY_TICKS)
					peak = std::max<int>(peak, window.count);
			}
			return peak;
		}

		// Stop at 200 percent of the limit, pause at 150 percent, and warn at 125 percent.
		RateAction action_for_peak(int peak, int limit) {
			if (limit == 0) return RateAction::NONE;
			if (peak >= limit * 2) return RateAction::STOP;
			if (peak * 2 >= limit * 3) return RateAction::PAUSE;
			if (peak * 4 >= limit * 5) return RateAction::WARN;
			return RateAction::NONE;
		}

		// The names Core0 uses in its rate-limit warning.
		std::string_view bucket_label(Bucket bucket) {
			switch (bucket) {
			case Bucket::GLOBAL:              return "overall RPC traffic";
			case Bucket::SCHEDULE:            return "schedule RPC traffic";
			case Bucket::NETWORK:             return "network RPC traffic";
			case Bucket::LOGGING:             return "logging RPC traffic";
			case Bucket::RFID:                return "RFID RPC traffic";
			case Bucket::BELT:                return "belt RPC traffic";
			case Bucket::SERVO:               return "servo RPC traffic";
			case Bucket::PLATFORM_MANAGEMENT: return "platform management RPC traffic";
			case Bucket::HEARTBEAT:           return "heartbeat RPC traffic";
			case Bucket::EVENTS:              return "event RPC traffic";
			case Bucket::LASER:               return "laser RPC traffic";
			case Bucket::LED:                 return "LED RPC traffic";
			case Bucket::CV_SENSOR:           return "CV sensor RPC traffic";
			case Bucket::ENTROPY:             return "entropy RPC traffic";
			case Bucket::ATTESTATION:         return "attestation RPC traffic";
			case Bucket::COUNT:               break;
			}
			return "this RPC command";
		}

		// Attestation tags are HMAC-SHA-256 (RFC 2104) over this domain, the proof without its tag, and
		// the message, truncated to RPC_ATTEST_TAG_SIZE bytes.
		constexpr std::string_view ATTESTATION_DOMAIN = "quercus.app-attestation";
		constexpr std::size_t      ATTESTATION_HEADER = offsetof(rpc_attest_proof_t, tag);
		constexpr std::size_t      SHA256_BLOCK_SIZE  = 64;

		std::array<uint8_t, RPC_ATTEST_TAG_SIZE> attestation_tag(const rpc_attest_proof_t& proof,
																 std::span<const uint8_t>  message) {
			const auto key_pad = [](uint8_t pad) {
				std::vector<uint8_t> block(SHA256_BLOCK_SIZE, pad);
				for (std::size_t index = 0; index < ATTESTATION_TEST_KEY.size(); index++)
					block[index] ^= ATTESTATION_TEST_KEY[index];
				return block;
			};

			std::vector<uint8_t> inner = key_pad(0x36);
			inner.insert(inner.end(), ATTESTATION_DOMAIN.begin(), ATTESTATION_DOMAIN.end());
			const auto* header = reinterpret_cast<const uint8_t*>(&proof);
			inner.insert(inner.end(), header, header + ATTESTATION_HEADER);
			inner.insert(inner.end(), message.begin(), message.end());
			const std::array<uint8_t, 32> inner_digest = seed::sha256(inner.data(), inner.size());

			std::vector<uint8_t> outer = key_pad(0x5c);
			outer.insert(outer.end(), inner_digest.begin(), inner_digest.end());
			const std::array<uint8_t, 32> digest = seed::sha256(outer.data(), outer.size());

			std::array<uint8_t, RPC_ATTEST_TAG_SIZE> tag{};
			std::copy_n(digest.begin(), tag.size(), tag.begin());
			return tag;
		}

		bool tags_equal(const uint8_t* left, const uint8_t* right) {
			uint8_t difference = 0;
			for (std::size_t index = 0; index < RPC_ATTEST_TAG_SIZE; index++) difference |= left[index] ^ right[index];
			return difference == 0;
		}

		template <typename T> std::vector<uint8_t> struct_payload(const T& value) {
			const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
			return {bytes, bytes + sizeof(value)};
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
		case Bucket::ENTROPY:             return "entropy";
		case Bucket::ATTESTATION:         return "attestation";
		case Bucket::COUNT:               break;
		}
		return "global";
	}

	int capacity_of(Bucket bucket) {
		// Requests per simulated second, aligned with the physical platform's buckets.
		switch (bucket) {
		case Bucket::GLOBAL:              return 100;
		case Bucket::SCHEDULE:            return 2;
		case Bucket::ENTROPY:             return 2;
		case Bucket::NETWORK:             return 5;
		case Bucket::LOGGING:             return 5;
		case Bucket::RFID:                return 5;
		case Bucket::PLATFORM_MANAGEMENT: return 5;
		case Bucket::BELT:                return 10;
		case Bucket::SERVO:               return 10;
		case Bucket::CV_SENSOR:           return 10;
		case Bucket::HEARTBEAT:           return 25;
		case Bucket::EVENTS:              return 25;
		case Bucket::ATTESTATION:         return 25;
		case Bucket::LASER:               return 50;
		case Bucket::LED:                 return 50;
		case Bucket::COUNT:               break;
		}
		return 0;
	}

	Bucket request_bucket(uint8_t cmd) {
		return bucket_for(cmd);
	}

	int request_cost(uint8_t cmd) {
		// A full RFID read replaces 32 page reads with one exchange, so it costs 3/5 of the RFID bucket.
		if (cmd == CMD_RFID_READ_DATA_FULL) return std::max(1, capacity_of(Bucket::RFID) * 3 / 5);
		return 1;
	}

	Runtime* Manager::find(module::ModuleId id) {
		const auto found = runtimes_.find(id.raw());
		return found == runtimes_.end() ? nullptr : &found->second;
	}

	const Runtime* Manager::find(module::ModuleId id) const {
		const auto found = runtimes_.find(id.raw());
		return found == runtimes_.end() ? nullptr : &found->second;
	}

	Manager::Manager(world::State& world, module::Changes* changes)
		: network_(static_cast<uint64_t>(settings::get_setting<int>("seed")), net::config_from_settings()),
		  log_network_(static_cast<uint64_t>(settings::get_setting<int>("seed")), log_channel_config()),
		  master_seed_(static_cast<uint64_t>(settings::get_setting<int>("seed"))),
		  laser_event_extra_delay_ms_(static_cast<uint64_t>(settings::get_setting<int>("laser_event_extra_delay_ms"))),
		  rfid_event_delay_mean_ms_(settings::get_setting<double>("rfid_event_extra_delay_mean_ms")),
		  rfid_event_delay_sd_ms_(settings::get_setting<double>("rfid_event_extra_delay_sd_ms")), world_(&world),
		  changes_(changes) {
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

	void Manager::defer_violation(Runtime& runtime, Bucket bucket, uint64_t tick, RateViolation& violation) {
		const int        limit  = capacity_of(bucket);
		const int        peak   = peak_count(runtime.rate_states[static_cast<std::size_t>(bucket)], tick);
		const RateAction action = action_for_peak(peak, limit);
		if (action == RateAction::NONE) return;

		// Equal actions go to the bucket that is furthest over its limit.
		if (action > violation.action ||
			(action == violation.action && peak * violation.limit > violation.peak * limit)) {
			violation = RateViolation{action, bucket, limit, peak};
		}
		if (!runtime.last_rate_warning_tick || tick - *runtime.last_rate_warning_tick >= RATE_LIMIT_WARNING_TICKS) {
			runtime.pending_rate_warning   = RateViolation{action, bucket, limit, peak};
			runtime.last_rate_warning_tick = tick;
		}
	}

	bool Manager::enforce(module::ModuleId id, const RateViolation& violation, uint64_t tick) {
		if (violation.action == RateAction::PAUSE) {
			pause_application(id, tick);
			diagnostics_.push_back(Diagnostic{tick, id, violation.bucket, "pause", violation.peak, violation.limit});
		} else if (violation.action == RateAction::STOP) {
			stop_application(tick, id,
							 to_string(violation.bucket) + " bucket exceeded its stop threshold (" +
								 std::to_string(violation.peak) + "/" + std::to_string(violation.limit) + ")",
							 violation.bucket, false);
			return true;
		}
		return false;
	}

	IngressAdmission Manager::account_ingress(module::ModuleId id, ModuleTime time, std::span<const uint8_t> frame) {
		Runtime* runtime = find(id);
		if (runtime == nullptr || runtime->lifecycle == Lifecycle::STOPPED) return {};
		const uint64_t tick = time.global_tick;
		RateViolation  violation;

		RateState& global = runtime->rate_states[static_cast<std::size_t>(Bucket::GLOBAL)];
		record_request(global, tick, 1);
		if (global.current.count > capacity_of(Bucket::GLOBAL))
			defer_violation(*runtime, Bucket::GLOBAL, tick, violation);

		// Only a well-formed request with known flags is charged to its category.
		rpc_message_t header{};
		if (frame.size() >= rpc::HEADER_SIZE) std::memcpy(&header, frame.data(), sizeof(header));
		const bool request = frame.size() >= rpc::HEADER_SIZE && header.type == MT_REQ && header.payload_len >= 0 &&
							 header.payload_len < RPC_MAX_PAYLOAD &&
							 frame.size() == rpc::HEADER_SIZE + static_cast<std::size_t>(header.payload_len) &&
							 (header.flags & ~RPC_FLAG_NON_CON) == 0;
		// Unknown commands have no category.
		if (const Bucket bucket = request_bucket(header.cmd); request && bucket != Bucket::GLOBAL) {
			RateState& state = runtime->rate_states[static_cast<std::size_t>(bucket)];
			record_request(state, tick, request_cost(header.cmd));
			if (state.current.count > capacity_of(bucket)) defer_violation(*runtime, bucket, tick, violation);
		}
		if (request && header.cmd == CMD_HEARTBEAT && header.payload_len == 0) note_heartbeat(id, tick);

		return IngressAdmission{enforce(id, violation, tick)};
	}

	Manager::ChargeResult Manager::admit(module::ModuleId id, uint8_t cmd, ModuleTime time) {
		Runtime* runtime = find(id);
		if (runtime == nullptr || runtime->lifecycle == Lifecycle::STOPPED) return {};
		const uint64_t tick    = time.global_tick;
		bool           allowed = true;
		RateViolation  violation;

		for (const Bucket bucket : {Bucket::GLOBAL, request_bucket(cmd)}) {
			RateState& state = runtime->rate_states[static_cast<std::size_t>(bucket)];
			refresh_window(state, tick);
			if (state.current.count > capacity_of(bucket)) {
				allowed = false;
				defer_violation(*runtime, bucket, tick, violation);
			}
			// Unknown commands have no category, so only the global bucket applies.
			if (bucket == Bucket::GLOBAL && request_bucket(cmd) == Bucket::GLOBAL) break;
		}

		// Core0 sends a pending warning to the application's log with the next admission check.
		if (runtime->pending_rate_warning) {
			const RateViolation warning = *runtime->pending_rate_warning;
			runtime->pending_rate_warning.reset();
			const std::string text = "\x1b[1;38;5;214m[RPC rate limit] " + std::string(bucket_label(warning.bucket)) +
									 " exceeded its limit. Allowed: " + std::to_string(warning.limit) +
									 " requests/s, peak: " + std::to_string(warning.peak) + "\x1b[0m\n";
			log_network_.send(tick, id, {id},
							  tag_diagnostic(DIAGNOSTIC_KIND_LOG, std::vector<uint8_t>(text.begin(), text.end())));
			diagnostics_.push_back(Diagnostic{tick, id, warning.bucket, "warning", warning.peak, warning.limit});
		}

		const bool stopped = enforce(id, violation, tick);
		return ChargeResult{allowed && !stopped, stopped};
	}

	void Manager::note_heartbeat(module::ModuleId id, uint64_t global_tick) {
		Runtime* runtime = find(id);
		if (runtime == nullptr || runtime->lifecycle == Lifecycle::STOPPED) return;
		runtime->heartbeat_seen             = true;
		runtime->last_heartbeat_global_tick = global_tick;
		runtime->heartbeat_warned           = false;
		if (runtime->lifecycle == Lifecycle::ACTIVATING) {
			runtime->lifecycle = Lifecycle::RUNNING;
		}
	}

	void Manager::application_launched(module::ModuleId id, const ImageDigest& image_digest) {
		Runtime* runtime = find(id);
		if (runtime == nullptr) return;
		std::array<uint8_t, RPC_ATTEST_FINGERPRINT_SIZE> fingerprint{};
		std::copy_n(image_digest.begin(), fingerprint.size(), fingerprint.begin());
		runtime->app_fingerprint = fingerprint;
	}

	void Manager::pause_application(module::ModuleId id, uint64_t tick) {
		Runtime* runtime_ptr = find(id);
		if (runtime_ptr == nullptr || runtime_ptr->lifecycle == Lifecycle::STOPPED) return;
		Runtime& runtime = *runtime_ptr;

		// Repeated enforcement while a resume is pending extends the physical delayed work.
		runtime.paused_until_global_tick = tick + PAUSE_DURATION_TICKS;
		next_resume_tick_ = std::min(next_resume_tick_.value_or(UINT64_MAX), runtime.paused_until_global_tick);
		if (runtime.pause_resume_pending) return;

		if (module::Module* physical = world_->module(id)) {
			if (changes_) changes_->set(id.raw());
			runtime.restore_big_belt_speed   = physical->big_belt_speed();
			runtime.restore_small_belt_speed = physical->small_belt_speed();
			runtime.belt_restore_valid       = true;
			physical->set_big_belt_speed(module::BeltSpeed{});
			physical->set_small_belt_speed(module::BeltSpeed{});
		}
		runtime.pause_resume_pending = true;
		runtime.lifecycle            = Lifecycle::PAUSED;
	}

	void Manager::prepare_tick(uint64_t tick) {
		if (!next_resume_tick_ || tick < *next_resume_tick_) return;
		next_resume_tick_.reset();
		for (const module::ModuleId id : modules_) {
			Runtime& runtime = *find(id);
			if (runtime.lifecycle == Lifecycle::PAUSED && runtime.pause_resume_pending &&
				tick >= runtime.paused_until_global_tick) {
				if (module::Module* physical = world_->module(id); physical != nullptr && runtime.belt_restore_valid) {
					if (changes_) changes_->set(id.raw());
					physical->set_big_belt_speed(runtime.restore_big_belt_speed);
					physical->set_small_belt_speed(runtime.restore_small_belt_speed);
				}
				runtime.belt_restore_valid   = false;
				runtime.pause_resume_pending = false;
				runtime.lifecycle            = runtime.heartbeat_seen ? Lifecycle::RUNNING : Lifecycle::ACTIVATING;
				diagnostics_.push_back(Diagnostic{tick, id, Bucket::GLOBAL, "resume", 0, 0});
			}
			if (runtime.pause_resume_pending)
				next_resume_tick_ = std::min(next_resume_tick_.value_or(UINT64_MAX), runtime.paused_until_global_tick);
		}
	}

	void Manager::stop_application(uint64_t tick, module::ModuleId id, std::string reason, Bucket cause,
								   bool heartbeat_failure, bool diagnose) {
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
		runtime.heartbeat_failed           = heartbeat_failure;
		runtime.rate_stopped               = !heartbeat_failure;
		runtime.crashed                    = false;
		runtime.paused_until_global_tick   = 0;
		runtime.pause_resume_pending       = false;
		runtime.belt_restore_valid         = false;
		runtime.rate_states                = {};
		runtime.last_rate_warning_tick.reset();
		runtime.pending_rate_warning.reset();
		runtime.last_left_clear  = false;
		runtime.last_right_clear = false;
		runtime.last_rfid_uid.clear();
		runtime.app_fingerprint.reset();
		runtime.outbound.clear();

		std::erase_if(pending_hardware_events_, [id](const PendingHardwareEvent& event) { return event.module == id; });

		// Core0's safe stop affects this module's outputs only. Sensors and accumulated physical
		// state remain part of the world, while moving/energized actuators are made safe.
		if (module::Module* physical = world_->module(id)) {
			if (changes_) changes_->set(id.raw());
			physical->set_big_belt_speed(module::BeltSpeed{});
			physical->set_small_belt_speed(module::BeltSpeed{});
			physical->reset_belt_travel();
			physical->set_servo_angle(module::ServoAngle{});
			physical->set_led_color(module::LedColor{});
			physical->set_left_laser_enabled(false);
			physical->set_right_laser_enabled(false);
		}

		if (diagnose) diagnostics_.push_back(Diagnostic{tick, id, cause, "stop", 0, 0});
	}

	void Manager::application_crashed(uint64_t tick, module::ModuleId id, std::string reason) {
		Runtime* runtime = find(id);
		if (runtime == nullptr || runtime->lifecycle == Lifecycle::STOPPED) return;
		// The runtime prints the crash report. A bucket "stop" diagnostic would misname the cause.
		stop_application(tick, id, "crashed: " + reason, Bucket::GLOBAL, false, false);
		runtime->rate_stopped = false;
		runtime->crashed      = true;
	}

	std::vector<module::ModuleId> Manager::check_supervision(uint64_t tick) {
		std::vector<module::ModuleId> stopped;
		for (const module::ModuleId id : modules_) {
			Runtime& runtime = *find(id);

			if (runtime.lifecycle == Lifecycle::STOPPED) {
				if (!runtime.heartbeat_failed && !runtime.rate_stopped && !runtime.crashed) {
					throw std::runtime_error("Module " + std::to_string(id.raw()) +
											 " was stopped: " + runtime.stop_reason);
				}
				if (runtime.heartbeat_failed) stopped.push_back(id);
				continue;
			}

			// A paused application gets no ticks, so it cannot be expected to do anything.
			if (runtime.lifecycle == Lifecycle::PAUSED) {
				continue;
			}

			if (!runtime.heartbeat_seen) {
				if (tick >= ACTIVATION_DEADLINE_TICKS) {
					stop_application(tick, id, "did not send a first heartbeat before its activation deadline",
									 Bucket::HEARTBEAT, true);
					stopped.push_back(id);
				}
				continue;
			}

			const uint64_t silence = tick - runtime.last_heartbeat_global_tick;
			if (silence > HEARTBEAT_FAILURE_TICKS) {
				stop_application(tick, id, "stopped sending heartbeats", Bucket::HEARTBEAT, true);
				stopped.push_back(id);
				continue;
			}
			if (silence > HEARTBEAT_WARNING_TICKS && !runtime.heartbeat_warned) {
				runtime.heartbeat_warned = true;
				diagnostics_.push_back(Diagnostic{tick, id, Bucket::HEARTBEAT, "heartbeat_late", 0, 0});
			}
		}

		return stopped;
	}

	std::vector<Diagnostic> Manager::take_diagnostics() {
		std::vector<Diagnostic> taken;
		taken.swap(diagnostics_);
		return taken;
	}

	std::size_t Manager::rate_sample_count(module::ModuleId id, Bucket bucket) const {
		const Runtime* runtime = find(id);
		if (runtime == nullptr || bucket >= Bucket::COUNT) {
			return 0;
		}
		return runtime->rate_states[static_cast<std::size_t>(bucket)].current.count;
	}

	std::optional<rpc::Frame> Manager::handle_request(module::ModuleId id, const rpc::Frame& request, ModuleTime time,
													  std::optional<IngressAdmission> ingress) {
		const IngressAdmission ingress_admission =
			ingress.has_value() ? *ingress : account_ingress(id, time, rpc::encode(request));
		Runtime* runtime_ptr = find(id);
		if (runtime_ptr == nullptr) {
			return {};
		}
		Runtime& runtime = *runtime_ptr;
		if (runtime.lifecycle == Lifecycle::STOPPED && !ingress_admission.stop_triggered) {
			return {};
		}
		if (ingress_admission.stop_triggered) {
			if ((request.flags & RPC_FLAG_NON_CON) != 0) return {};
			rpc::Frame response;
			response.type                = MT_RESP;
			response.cmd                 = request.cmd;
			response.call_id             = request.call_id;
			response.status              = STATUS_TRY_AGAIN;
			response.stop_after_delivery = true;
			return response;
		}

		// These permission gates precede command admission on physical Core0. The frame has already
		// consumed global and category capacity at ingress.
		if ((id == TOWER_MODULE_ID && is_hardware_command(request.cmd)) ||
			(id != TOWER_MODULE_ID && request.cmd == CMD_SET_FLIGHT_SCHEDULE)) {
			if ((request.flags & RPC_FLAG_NON_CON) != 0) {
				return {};
			}
			rpc::Frame response;
			response.type    = MT_RESP;
			response.cmd     = request.cmd;
			response.call_id = request.call_id;
			response.status  = STATUS_PERM;
			return response;
		}

		// Admission checks the buckets as they are now, including requests that arrived later.
		const ChargeResult admission = admit(id, request.cmd, time);
		if (!admission.allowed) {
			// Over its limit: refuse rather than queue, matching what hardware would do.
			if ((request.flags & RPC_FLAG_NON_CON) != 0) {
				return {};
			}
			rpc::Frame response;
			response.type                = MT_RESP;
			response.cmd                 = request.cmd;
			response.call_id             = request.call_id;
			response.status              = STATUS_TRY_AGAIN;
			response.stop_after_delivery = admission.stop_triggered;
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

			const std::optional<uint32_t> mask = parse_native<uint32_t>(request.payload);
			if (!mask.has_value() || (mask.value() & ~SUPPORTED_EVENTS) != 0) {
				response.status = STATUS_INVALID;
			} else if (id == TOWER_MODULE_ID && (mask.value() & HARDWARE_EVENTS) != 0) {
				response.status = STATUS_PERM;
			} else if (request.cmd == CMD_SUBSCRIBE_TO_EVENT) {
				const uint32_t newly_subscribed = mask.value() & ~runtime.subscriptions;
				runtime.subscriptions |= mask.value();
				if ((newly_subscribed & EVENT_RFID_DETECT) != 0) {
					const module::Module* module = world_->module(id);
					const auto uid = module == nullptr ? std::span<const uint8_t>{} : visible_rfid_uid(*module);
					runtime.last_rfid_uid.assign(uid.begin(), uid.end());
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

			if (!is_valid_flight_board(request.payload)) {
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
			data = request.payload;
			recipients.reserve(modules_.size());
			for (const module::ModuleId id : modules_) {
				if (id != sender) recipients.push_back(id);
			}
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

	void Manager::advance(uint64_t tick, const module::Changes* changes) {
		for (const module::ModuleId id : modules_) {
			if (changes && !(*changes)[id.raw()]) continue;
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

			const auto uid_payload = visible_rfid_uid(*module);
			if (!std::ranges::equal(uid_payload, runtime.last_rfid_uid)) {
				runtime.last_rfid_uid.assign(uid_payload.begin(), uid_payload.end());
				// Only an arriving tag is worth an event; a departing one is observed by polling.
				if (!uid_payload.empty()) {
					schedule_hardware_event(tick, id, EVENT_RFID_DETECT, runtime.last_rfid_uid,
											sample_rfid_event_delay_ms(id));
				}
			}
		}

		deliver_hardware_events(tick);

		for (net::Delivery& delivery : network_.deliver_due(tick)) {
			publish(delivery.recipient, EVENT_MESSAGE_RECEIVED, std::move(delivery.payload));
		}

		for (net::Delivery& delivery : log_network_.deliver_due(tick)) {
			Runtime* runtime = find(delivery.recipient);
			if (runtime == nullptr || runtime->lifecycle == Lifecycle::STOPPED) {
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
		if (runtime == nullptr || (runtime->lifecycle == Lifecycle::STOPPED && !frame.stop_after_delivery)) {
			return;
		}
		runtime->outbound.push_back(std::move(frame));
	}

	std::vector<rpc::Frame> Manager::take_outbound(module::ModuleId id, std::size_t byte_budget) {
		std::vector<rpc::Frame> frames;
		Runtime*                runtime = find(id);
		if (runtime == nullptr || (runtime->lifecycle == Lifecycle::STOPPED &&
								   (runtime->outbound.empty() || !runtime->outbound.front().stop_after_delivery))) {
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

		if (changes_) changes_->set(id.raw());
		module::Module* module = world_->module(id);
		if (module == nullptr) {
			response.status = STATUS_NO_ENTRY;
			return response;
		}

		switch (request.cmd) {
		case CMD_LASER_LEFT_DETECT:
		case CMD_LASER_RIGHT_DETECT:
		case CMD_LASER_LEFT_GET:
		case CMD_LASER_RIGHT_GET:
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			if (request.cmd == CMD_LASER_LEFT_DETECT) {
				response.payload = byte_payload(module->is_laser_clear(true) ? 1 : 0);
			} else if (request.cmd == CMD_LASER_RIGHT_DETECT) {
				response.payload = byte_payload(module->is_laser_clear(false) ? 1 : 0);
			} else if (request.cmd == CMD_LASER_LEFT_GET) {
				response.payload = byte_payload(module->is_left_laser_enabled() ? 1 : 0);
			} else {
				response.payload = byte_payload(module->is_right_laser_enabled() ? 1 : 0);
			}
			break;
		case CMD_LASER_LEFT_SET:
			if (request.payload.size() != 1 || request.payload[0] > 1) {
				response.status = STATUS_INVALID;
				break;
			}
			module->set_left_laser_enabled(request.payload[0] == 1);
			break;
		case CMD_LASER_RIGHT_SET:
			if (request.payload.size() != 1 || request.payload[0] > 1) {
				response.status = STATUS_INVALID;
				break;
			}
			module->set_right_laser_enabled(request.payload[0] == 1);
			break;

		case CMD_BELT_BIG_GET_SPEED:
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			response.payload = native_payload(static_cast<int32_t>(std::lround(module->big_belt_speed().raw())));
			break;
		case CMD_BELT_SMALL_GET_SPEED:
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			response.payload = native_payload(static_cast<int32_t>(std::lround(module->small_belt_speed().raw())));
			break;

		case CMD_BELT_BIG_GET_ENCODER_COUNT:
		case CMD_BELT_SMALL_GET_ENCODER_COUNT: {
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			const world::Calibration& calibration = world::calibration_for(id);
			const int64_t travel = request.cmd == CMD_BELT_BIG_GET_ENCODER_COUNT ? module->big_belt_travel_um()
																				 : module->small_belt_travel_um();
			response.payload     = native_payload(static_cast<int32_t>(world::encoder_count(travel, calibration)));
			break;
		}
		case CMD_BELT_BIG_GET_ENCODER_FREQ:
		case CMD_BELT_SMALL_GET_ENCODER_FREQ: {
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			const world::Calibration& calibration = world::calibration_for(id);
			const bool                big         = request.cmd == CMD_BELT_BIG_GET_ENCODER_FREQ;
			const double              speed = big ? module->big_belt_speed().raw() : module->small_belt_speed().raw();
			const world::BeltCurve&   curve = big ? calibration.mechanical.big_belt : calibration.mechanical.small_belt;
			const uint32_t            frequency_millihz =
				static_cast<uint32_t>(std::llround(world::encoder_frequency_hz(speed, curve, calibration) * 1000.0));
			response.payload = native_payload(frequency_millihz);
			break;
		}

		case CMD_CV_SENSOR_GET_CURRENT:
		case CMD_CV_SENSOR_GET_POWER:
		case CMD_CV_SENSOR_GET_VOLTAGE: {
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			const world::Electrics electrics = world::electrics_of(*module, world::calibration_for(id));
			const double           value     = request.cmd == CMD_CV_SENSOR_GET_CURRENT ? electrics.current_a
											   : request.cmd == CMD_CV_SENSOR_GET_POWER ? electrics.power_w
																						: electrics.voltage_v;
			response.payload                 = native_payload(static_cast<float>(value));
			break;
		}
		case CMD_BELT_BIG_SET_SPEED:
		case CMD_BELT_SMALL_SET_SPEED: {
			const std::optional<int32_t> speed = parse_native<int32_t>(request.payload);
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
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			response.payload = native_payload(static_cast<int32_t>(std::lround(module->servo_angle().raw())));
			break;
		case CMD_SERVO_ANGLE_SET: {
			const std::optional<int32_t> angle = parse_native<int32_t>(request.payload);
			if (!angle.has_value() || angle.value() < 0 || angle.value() > 180) {
				response.status = STATUS_INVALID;
				break;
			}
			// A servo has no feedback, so the reported angle is always the commanded one.
			world::command_servo(*module, module::ServoAngle{static_cast<double>(angle.value())});
			break;
		}

		case CMD_LED_GET_COLOR: {
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			response.payload = native_payload(module->led_color().raw() & 0x00FFFFFFU);
			break;
		}
		case CMD_LED_SET_COLOR: {
			const std::optional<uint32_t> color = parse_native<uint32_t>(request.payload);
			if (!color.has_value()) {
				response.status = STATUS_INVALID;
				break;
			}
			module->set_led_color(module::LedColor{color.value() & 0x00FFFFFFU});
			break;
		}

		case CMD_RFID_CHECK_TAG:
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			response.payload = byte_payload(module->is_rfid_active() ? 1 : 0);
			break;
		case CMD_RFID_GET_UID:
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
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
				response.status = STATUS_IO;
				break;
			}
			const RfidPage value = module->rfid_page(page);
			response.payload.assign(value.begin(), value.end());
			break;
		}
		case CMD_RFID_READ_DATA_FULL:
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			if (!module->is_rfid_active()) {
				response.status = STATUS_IO;
				break;
			}
			response.payload.reserve(RFID_PAYLOAD_PAGE_COUNT * RFID_PAGE_SIZE_BYTES);
			for (std::size_t page = 0; page < RFID_PAYLOAD_PAGE_COUNT; page++) {
				const RfidPage value = module->rfid_page(page);
				response.payload.insert(response.payload.end(), value.begin(), value.end());
			}
			break;
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
				response.status = STATUS_IO;
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

		case CMD_GET_OWN_ID:
		case CMD_GET_NETWORK_MAP:
		case CMD_GET_MODULE_LIST:
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			if (request.cmd == CMD_GET_OWN_ID) {
				response.payload = byte_payload(id.raw());
			} else if (request.cmd == CMD_GET_NETWORK_MAP) {
				response.payload.assign(network_map_.begin(), network_map_.end());
			} else {
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
			}
			break;

		case CMD_ENTROPY_GET: {
			if (!request.payload.empty()) {
				response.status = STATUS_INVALID;
				break;
			}
			// Core0 serves CSPRNG output. The simulator draws a deterministic stream per module and call.
			Runtime&         runtime = *find(id);
			seed::SplitMix64 stream(seed::derive(master_seed_, "entropy/" + std::to_string(id.raw()) + "/" +
																   std::to_string(runtime.entropy_draws++)));
			response.payload.resize(ENTROPY_RESPONSE_SIZE);
			for (std::size_t offset = 0; offset < response.payload.size(); offset += sizeof(uint64_t)) {
				const uint64_t value = stream.next();
				for (std::size_t byte = 0; byte < sizeof(uint64_t) && offset + byte < response.payload.size(); byte++)
					response.payload[offset + byte] = static_cast<uint8_t>(value >> (8 * byte));
			}
			break;
		}
		case CMD_ATTEST:        response.status = attest(id, request.payload, response.payload); break;
		case CMD_ATTEST_VERIFY: response.status = attest_verify(id, request.payload, response.payload); break;

		case CMD_HEARTBEAT:
			// Liveness was recorded at ingress, so a queued heartbeat cannot refresh it later.
		case CMD_LOG:
		case CMD_PANIC_REPORT:
			// Already recorded before dispatch.
			break;

		default: response.status = STATUS_NO_SYS; break;
		}

		return response;
	}

	int16_t Manager::attest(module::ModuleId id, const std::vector<uint8_t>& message,
							std::vector<uint8_t>& response) const {
		const Runtime& runtime = *find(id);
		if (message.size() > RPC_ATTEST_MAX_MESSAGE) return STATUS_MSG_SIZE;
		// Simulated modules have no debugger that could taint an application (-EACCES).
		if (!runtime.app_fingerprint) return STATUS_TRY_AGAIN;

		rpc_attest_proof_t proof{};
		proof.version   = RPC_ATTEST_VERSION;
		proof.module_id = id.raw();
		std::ranges::copy(*runtime.app_fingerprint, proof.app_fingerprint);
		std::ranges::copy(attestation_tag(proof, message), proof.tag);
		response = struct_payload(proof);
		return STATUS_OK;
	}

	int16_t Manager::attest_verify(module::ModuleId id, const std::vector<uint8_t>& request,
								   std::vector<uint8_t>& response) const {
		// The request is message || rpc_attest_proof_t.
		if (request.size() < sizeof(rpc_attest_proof_t)) return STATUS_INVALID;
		const std::size_t  message_length = request.size() - sizeof(rpc_attest_proof_t);
		rpc_attest_proof_t proof;
		std::memcpy(&proof, request.data() + message_length, sizeof(proof));

		if (message_length > RPC_ATTEST_MAX_MESSAGE) return STATUS_MSG_SIZE;
		if (proof.version != RPC_ATTEST_VERSION || proof.module_id == 0) return STATUS_BAD_MSG;
		const auto tag = attestation_tag(proof, std::span(request.data(), message_length));
		if (!tags_equal(tag.data(), proof.tag)) return STATUS_BAD_MSG;

		const Runtime&      runtime = *find(id);
		rpc_attest_result_t result{};
		result.module_id = proof.module_id;
		std::ranges::copy(proof.app_fingerprint, result.app_fingerprint);
		result.same_app =
			runtime.app_fingerprint.has_value() && std::ranges::equal(*runtime.app_fingerprint, proof.app_fingerprint);
		response = struct_payload(result);
		return STATUS_OK;
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
		if (!next_resume_tick_) return false;
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
