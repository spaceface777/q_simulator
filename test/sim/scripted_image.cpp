#include "../../src/rpc/frame.h"
#include <array>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <quercus/sim_instance.h>
#include <quercus/sim_transport.h>
#include <string>

namespace test_app {
	namespace {
		unsigned number(const std::string& name, unsigned fallback) {
			const char* value = std::getenv(name.c_str());
			return value && *value ? static_cast<unsigned>(std::strtoul(value, nullptr, 0)) : fallback;
		}

		unsigned option(const char* name, unsigned id, unsigned fallback) {
			const std::string key = std::string("TEST_APP_") + name;
			return number(key + "_" + std::to_string(id), number(key, fallback));
		}

		void record(const rpc::Frame& frame, unsigned tick, unsigned id) {
			if (const char* path = std::getenv("TEST_APP_RECORD")) {
				std::ofstream file(path, std::ios::app);
				if (number("TEST_APP_RECORD_TICKS", 0)) file << tick << ':';
				file << static_cast<unsigned>(frame.type) << ':' << static_cast<unsigned>(frame.cmd) << '\n';
			}
			if (const char* path = std::getenv("TEST_APP_STATUS_RECORD"); path && id == 1 && frame.type == MT_RESP) {
				std::ofstream file(path, std::ios::app);
				file << static_cast<unsigned>(frame.cmd) << ':' << frame.status << '\n';
			}
		}

		template <typename T> std::vector<uint8_t> payload(T value) {
			std::vector<uint8_t> bytes(sizeof(value));
			std::memcpy(bytes.data(), &value, sizeof(value));
			return bytes;
		}

		struct State {
			unsigned id, ticks, heartbeat_every, heartbeat_until, subscribe, exit_after, query_tick, ingress, burst;
			bool     startup_rpc, startup_sent, startup_done, heartbeat_sent, reply, never_activate;
			unsigned mutant;
			uint16_t call_id;
			uint64_t now;
			qsim_outcome_t                                  outcome;
			std::array<uint8_t, QSIM_MAX_BATCH_BYTES>       bytes;
			std::array<qsim_frame_t, QSIM_MAX_GRANT_FRAMES> frames;
			uint32_t                                        count, used;
		};

		static_assert(std::is_trivially_copyable_v<State>);

		struct App {
			static inline State state{};

			static qsim_status_t bind(const qsim_image_config_t* config) {
				state.id              = config->module_id;
				state.call_id         = 0x8000;
				state.heartbeat_every = option("HEARTBEAT_EVERY", state.id, 50);
				state.heartbeat_until = option("HEARTBEAT_UNTIL", state.id, UINT_MAX);
				state.subscribe       = option("SUBSCRIBE", state.id, 0);
				state.exit_after      = option("EXIT_AFTER", state.id, 0);
				state.query_tick      = option("QUERY_TICK", state.id, UINT_MAX);
				state.ingress         = option("INGRESS_PROBE", state.id, 0);
				state.burst           = option("RATE_BURST", state.id, 0);
				state.startup_rpc     = option("STARTUP_RPC", state.id, 0);
				state.startup_done    = !state.startup_rpc;
				state.never_activate  = option("NEVER_ACTIVATE", state.id, 0);
				state.reply           = option("REPLY_TO_MESSAGE", state.id, 0) && state.id == 1;
				const char* mutant    = std::getenv("TEST_APP_MUTANT");
				if (mutant) {
					if (!std::strcmp(mutant, "reverse_leg")) state.mutant = 1;
					if (!std::strcmp(mutant, "wrong_laser")) state.mutant = 2;
					if (!std::strcmp(mutant, "forbidden_colour")) state.mutant = 3;
				}
				return QSIM_STATUS_OK;
			}

			static bool append(const std::vector<uint8_t>& bytes) {
				if (state.count == state.frames.size() || state.used + bytes.size() > state.bytes.size()) return false;
				auto* start = state.bytes.data() + state.used;
				std::memcpy(start, bytes.data(), bytes.size());
				state.frames[state.count++] = {start, static_cast<uint32_t>(bytes.size())};
				state.used += static_cast<uint32_t>(bytes.size());
				return true;
			}

			static bool request(uint8_t cmd, const std::vector<uint8_t>& data = {}) {
				rpc::Frame frame;
				frame.type    = MT_REQ;
				frame.cmd     = cmd;
				frame.call_id = state.call_id++;
				frame.payload = data;
				return append(rpc::encode(frame));
			}

			static qsim_status_t run(const qsim_grant_t* grant, qsim_grant_result_t* result) {
				if (grant->until_us <= grant->start_us) return QSIM_STATUS_INVALID_ARGUMENT;
				state.count = state.used = 0;
				state.now                = grant->until_us;
				if (const char* path = std::getenv("TEST_APP_GRANT_RECORD")) {
					std::ofstream file(path, std::ios::app);
					file << state.id << ':' << grant->start_us << ':' << grant->until_us << ':' << grant->inbound_count
						 << '\n';
				}
				if (state.exit_after && state.ticks >= state.exit_after) {
					*result = {QSIM_OUTCOME_NATIVE_FATAL, UINT64_MAX, nullptr, 0, "scripted exit"};
					return QSIM_STATUS_OK;
				}
				bool message = false;
				for (unsigned i = 0; i < grant->inbound_count; ++i) {
					rpc::FrameReader reader;
					reader.append(grant->inbound_frames[i].data, grant->inbound_frames[i].size);
					rpc::Frame frame;
					if (reader.next(frame) != rpc::DecodeStatus::OK) return QSIM_STATUS_IO_ERROR;
					record(frame, state.ticks, state.id);
					if (frame.type == MT_RESP && frame.cmd == CMD_GET_OWN_ID && frame.status == 0 &&
						frame.payload == std::vector<uint8_t>{static_cast<uint8_t>(state.id)})
						state.startup_done = true;
					if (frame.type == MT_EVENT && frame.cmd == EVENT_MESSAGE_RECEIVED) message = true;
				}
				bool accepted = true;
				if (state.reply && message) accepted &= request(CMD_SEND_PACKET, {2, 'r', 'e', 'p', 'l', 'y'});
				if (!state.ticks && state.subscribe)
					accepted &= request(CMD_SUBSCRIBE_TO_EVENT, payload(state.subscribe));
				if (state.startup_rpc && !state.startup_sent) {
					accepted &= request(CMD_GET_OWN_ID);
					state.startup_sent = true;
				}
				const bool startup  = state.startup_done && !state.heartbeat_sent;
				const bool periodic = !state.startup_rpc && state.heartbeat_every &&
									  state.ticks <= state.heartbeat_until && state.ticks % state.heartbeat_every == 0;
				if (!state.never_activate && (startup || periodic)) {
					accepted &= request(CMD_HEARTBEAT);
					state.heartbeat_sent = true;
				}
				const unsigned burst = (startup || periodic) && state.burst >= QSIM_MAX_GRANT_FRAMES
										   ? QSIM_MAX_GRANT_FRAMES - 1
										   : state.burst;
				for (unsigned i = 0; i < burst; ++i) accepted &= request(CMD_LASER_LEFT_GET);
				if (!state.ticks && state.ingress == 1) accepted &= append({0xa5});
				if (!state.ticks && state.ingress == 2) {
					rpc::Frame response;
					response.type    = MT_RESP;
					response.cmd     = CMD_HEARTBEAT;
					response.call_id = 0x1234;
					accepted &= append(rpc::encode(response));
				}
				if (state.ticks == state.query_tick) accepted &= request(CMD_LASER_LEFT_GET);
				if (state.ticks == 2) {
					if (state.mutant == 1) accepted &= request(CMD_BELT_BIG_SET_SPEED, payload<int32_t>(-100));
					if (state.mutant == 2) accepted &= request(CMD_LASER_RIGHT_SET, {1});
					if (state.mutant == 3) accepted &= request(CMD_LED_SET_COLOR, payload<uint32_t>(0xdeadbe));
				}
				*result = {QSIM_OUTCOME_GRANT_COMPLETE,
						   std::max<uint64_t>(state.now + 1, number("TEST_APP_IDLE_UNTIL", 0)), state.frames.data(),
						   state.count, nullptr};
				++state.ticks;
				return accepted ? QSIM_STATUS_OK : QSIM_STATUS_IO_ERROR;
			}

			static qsim_status_t destroy() { return QSIM_STATUS_OK; }
		};
	} // namespace
} // namespace test_app

extern "C" __attribute__((visibility("default"))) const qsim_image_api_t* qsim_image_api() {
	static const qsim_image_api_t api{QSIM_IMAGE_ABI_VERSION, test_app::App::bind, test_app::App::run,
									  test_app::App::destroy};
	return &api;
}
