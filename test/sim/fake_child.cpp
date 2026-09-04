// A stand-in Core 1 application, speaking the same contract as
// q_sdk/native_sim/lockstep_transport.c, for tests that cannot build a real applet.

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "../../src/childsync/barrier.h"
#include "../../src/rpc/frame.h"

namespace {

	// Long enough that a wait only expires when the parent has genuinely stopped ticking.
	constexpr std::chrono::microseconds PATIENT{1000000};

	unsigned env_number(const char* name, unsigned fallback) {
		const char* value = ::getenv(name);
		if (value == nullptr || *value == '\0') {
			return fallback;
		}
		return static_cast<unsigned>(std::strtoul(value, nullptr, 0));
	}

	unsigned module_env_number(const char* name, uint8_t id, unsigned fallback) {
		const std::string per_module = std::string(name) + "_" + std::to_string(id);
		const char*       value      = ::getenv(per_module.c_str());
		if (value == nullptr || *value == '\0') {
			return env_number(name, fallback);
		}
		return static_cast<unsigned>(std::strtoul(value, nullptr, 0));
	}

	void record(const char* path, const rpc::Frame& frame, unsigned tick, bool include_tick) {
		if (path == nullptr) {
			return;
		}
		std::ofstream file(path, std::ios::app);
		if (include_tick) file << tick << ":";
		file << static_cast<unsigned>(frame.type) << ":" << static_cast<unsigned>(frame.cmd) << "\n";
	}

	void record_status(const char* path, const rpc::Frame& frame) {
		if (path == nullptr || frame.type != MT_RESP) return;
		std::ofstream file(path, std::ios::app);
		file << static_cast<unsigned>(frame.cmd) << ":" << frame.status << "\n";
	}

	template <typename T> std::vector<uint8_t> native_payload(T value) {
		std::vector<uint8_t> payload(sizeof(value));
		std::memcpy(payload.data(), &value, sizeof(value));
		return payload;
	}

	bool append_request(childsync::Mailbox& outbound, uint16_t& call_id, uint8_t command,
						const std::vector<uint8_t>& payload) {
		rpc::Frame request;
		request.type                     = MT_REQ;
		request.cmd                      = command;
		request.call_id                  = call_id++;
		request.payload                  = payload;
		const std::vector<uint8_t> bytes = rpc::encode(request);
		return childsync::mailbox_append(outbound, bytes.data(), bytes.size()) == childsync::MailboxStatus::OK;
	}

	bool append_ingress_probe(childsync::Mailbox& outbound, unsigned kind) {
		if (kind == 1) {
			const uint8_t short_frame = 0xA5;
			return childsync::mailbox_append(outbound, &short_frame, sizeof(short_frame)) ==
				   childsync::MailboxStatus::OK;
		}
		if (kind == 2) {
			rpc::Frame response;
			response.type                    = MT_RESP;
			response.cmd                     = CMD_HEARTBEAT;
			response.call_id                 = 0x1234;
			const std::vector<uint8_t> bytes = rpc::encode(response);
			return childsync::mailbox_append(outbound, bytes.data(), bytes.size()) == childsync::MailboxStatus::OK;
		}
		return true;
	}

	// Mirrors the queueing the real adapter does: the page is copied out before the tick runs, so the
	// parent is free to refill it the moment the barrier releases.
	std::vector<rpc::Frame> take_inbound(childsync::Mailbox& mailbox, bool& corrupt) {
		std::vector<rpc::Frame> frames;
		corrupt = false;

		if (!childsync::mailbox_valid(mailbox)) {
			corrupt = true;
			return frames;
		}

		rpc::FrameReader  reader;
		const std::size_t count = childsync::mailbox_count(mailbox);
		for (std::size_t index = 0; index < count; index++) {
			std::size_t    length = 0;
			const uint8_t* bytes  = childsync::mailbox_read(mailbox, index, length);
			rpc::Frame     frame;
			reader.clear();
			reader.append(bytes, length);
			if (reader.next(frame) != rpc::DecodeStatus::OK) {
				corrupt = true;
				return frames;
			}
			frames.push_back(std::move(frame));
		}

		return frames;
	}

} // namespace

int main() {
	const char* control_fd = ::getenv("QSIM_CTRL_FD");
	const char* mailbox_fd = ::getenv("QSIM_MBOX_FD");
	const char* module_id  = ::getenv("QSIM_MODULE_ID");
	if (control_fd == nullptr || mailbox_fd == nullptr || module_id == nullptr) {
		std::fprintf(stderr, "fake_child: QSIM_CTRL_FD, QSIM_MBOX_FD and QSIM_MODULE_ID must be set\n");
		return 2;
	}

	const uint8_t id = static_cast<uint8_t>(std::strtoul(module_id, nullptr, 0));

	childsync::SharedControl shared(static_cast<int>(std::strtol(control_fd, nullptr, 0)));
	childsync::ChildSegment  segment(static_cast<int>(std::strtol(mailbox_fd, nullptr, 0)));
	childsync::Control&      control = shared.get();
	childsync::Segment&      pages   = segment.get();

	const unsigned heartbeat_every = module_env_number("FAKE_CHILD_HEARTBEAT_EVERY", id, 50); // 0 disables it.
	const unsigned heartbeat_until = module_env_number("FAKE_CHILD_HEARTBEAT_UNTIL", id, UINT_MAX);
	const unsigned subscribe_mask  = env_number("FAKE_CHILD_SUBSCRIBE", 0); // Mask to subscribe to on the first tick.
	const unsigned exit_after =
		env_number("FAKE_CHILD_EXIT_AFTER", 0); // Simulates a crash after N ticks. 0 runs forever.
	const unsigned    query_tick         = env_number("FAKE_CHILD_QUERY_TICK", UINT_MAX);
	const unsigned    ingress_probe      = module_env_number("FAKE_CHILD_INGRESS_PROBE", id, 0);
	const unsigned    rate_burst         = module_env_number("FAKE_CHILD_RATE_BURST", id, 0);
	const bool        startup_rpc        = env_number("FAKE_CHILD_STARTUP_RPC", 0) != 0;
	const bool        reply_to_message   = env_number("FAKE_CHILD_REPLY_TO_MESSAGE", 0) != 0 && id == 1;
	const char*       record_path        = ::getenv("FAKE_CHILD_RECORD"); // File to append observed commands to.
	const bool        record_ticks       = env_number("FAKE_CHILD_RECORD_TICKS", 0) != 0;
	const bool        certify_quiet      = env_number("FAKE_CHILD_CERTIFY_QUIET", 0) != 0;
	const char*       grant_record_path  = ::getenv("FAKE_CHILD_GRANT_RECORD");
	const char*       status_record_path = id == 1 ? ::getenv("FAKE_CHILD_STATUS_RECORD") : nullptr;
	const std::string mutant = ::getenv("FAKE_CHILD_MUTANT") == nullptr ? "" : ::getenv("FAKE_CHILD_MUTANT");

	uint16_t       call_id                = 0x8000;
	unsigned       ticks                  = 0;
	uint32_t       last_seen              = 0;
	uint64_t       local_time_us          = 0;
	const bool     selective_mode         = childsync::load_acquire(control.selective_enabled) != 0;
	const uint32_t scheduler_index        = control.scheduler_index[id];
	const uint32_t futex_bank             = scheduler_index / QUERCUS_FUTEX_BANK_SIZE;
	const uint32_t futex_mask             = UINT32_C(1) << (scheduler_index % QUERCUS_FUTEX_BANK_SIZE);
	bool           startup_rpc_sent       = false;
	bool           startup_rpc_done       = !startup_rpc;
	bool           startup_heartbeat_sent = false;

	// Announced only once the mapping is in place, so the parent never ticks an unmapped child.
	if (childsync::load_acquire(control.abi) != QUERCUS_SHM_ABI) {
		std::fprintf(stderr, "fake_child: segment ABI mismatch\n");
		return 2;
	}

	control.ready[id] = 1;

	while (true) {
		if (selective_mode) {
			while (childsync::load_acquire(control.wake_seq[id]) == last_seen) {
				const uint32_t bank_seen = childsync::load_acquire(control.futex_bank[futex_bank]);
				if (childsync::load_acquire(control.wake_seq[id]) != last_seen) break;
				(void)childsync::futex_wait_bitset(control.futex_bank[futex_bank], bank_seen, futex_mask, PATIENT);
			}
			last_seen = childsync::load_acquire(control.wake_seq[id]);
		} else {
			while (childsync::load_acquire(control.generation) == last_seen) {
				(void)childsync::futex_wait(control.generation, last_seen, PATIENT);
			}
			last_seen = childsync::load_acquire(control.generation);
		}

		if (selective_mode) {
			const quercus_selective_grant_t& selective = control.selective[id];
			if (selective.prefix_ticks > QUERCUS_RUNAHEAD_MAX_TICKS || selective.batch_ticks == 0 ||
				selective.batch_ticks > childsync::MAX_BATCH_TICKS)
				return 3;
			for (uint32_t index = 0; index < selective.prefix_ticks; index++) {
				if ((selective.prefix_active[index / 64U] & (UINT64_C(1) << (index % 64U))) == 0) continue;
				local_time_us += selective.prefix_delta_us[index];
				ticks++;
			}
		}

		if (!selective_mode && control.grant_mode == QUERCUS_GRANT_EXACT_RUNAHEAD) {
			const uint32_t runahead_ticks = control.runahead_ticks;
			if (!certify_quiet || runahead_ticks == 0 || runahead_ticks > QUERCUS_RUNAHEAD_MAX_TICKS) return 3;
			if (grant_record_path != nullptr && id == 1) {
				std::ofstream file(grant_record_path, std::ios::app);
				file << "R" << runahead_ticks << "\n";
			}
			for (uint32_t index = 0; index < runahead_ticks; index++) {
				const uint64_t word = control.runahead[id].active[index / 64U];
				if ((word & (UINT64_C(1) << (index % 64U))) == 0) continue;
				local_time_us += control.runahead[id].delta_us[index];
				ticks++;
			}

			quercus_runahead_certificate_t& certificate = control.certificate[id];
			certificate.now_us                          = local_time_us;
			certificate.next_event_us                   = UINT64_MAX;
			certificate.generation                      = last_seen;
			childsync::store_release(certificate.valid, 1);

			const uint32_t before = childsync::fetch_add_release(control.done_count, 1);
			if (before + 1 == childsync::load_acquire(control.expected)) {
				childsync::store_release(control.release, last_seen);
				(void)childsync::futex_wake(control.release, 1);
			}
			continue;
		}

		const uint32_t batch_ticks = selective_mode ? control.selective[id].batch_ticks : control.batch_ticks;
		if (batch_ticks == 0 || batch_ticks > childsync::MAX_BATCH_TICKS) return 3;
		const uint16_t active_mask = control.active_mask[id];
		if (grant_record_path != nullptr && id == 1) {
			std::ofstream file(grant_record_path, std::ios::app);
			file << batch_ticks << ":" << active_mask << "\n";
		}

		for (uint32_t batch_index = 0; batch_index < batch_ticks; batch_index++) {
			if ((active_mask & static_cast<uint16_t>(1U << batch_index)) != 0) {
				local_time_us += control.delta_us[id][batch_index];
				bool corrupt          = false;
				bool message_received = false;
				for (const rpc::Frame& frame : take_inbound(pages.to_child[batch_index], corrupt)) {
					// Anything inbound is queued for the application; here it is only recorded so
					// that a test can assert what the child was told.
					record(record_path, frame, ticks, record_ticks);
					record_status(status_record_path, frame);
					if (startup_rpc && frame.type == MT_RESP && frame.cmd == CMD_GET_OWN_ID && frame.status == 0 &&
						frame.payload.size() == 1 && frame.payload.front() == id) {
						startup_rpc_done = true;
					}
					if (frame.type == MT_EVENT && frame.cmd == EVENT_MESSAGE_RECEIVED) message_received = true;
				}
				if (corrupt) return 3;

				childsync::Mailbox& outbound = pages.to_parent[batch_index];
				childsync::mailbox_clear(outbound);
				if (reply_to_message && message_received &&
					!append_request(outbound, call_id, CMD_SEND_PACKET, {2, 'r', 'e', 'p', 'l', 'y'})) {
					return 4;
				}

				if (ticks == 0 && subscribe_mask != 0) {
					rpc::Frame subscribe;
					subscribe.type                   = MT_REQ;
					subscribe.cmd                    = CMD_SUBSCRIBE_TO_EVENT;
					subscribe.call_id                = call_id++;
					subscribe.payload                = native_payload(static_cast<uint32_t>(subscribe_mask));
					const std::vector<uint8_t> bytes = rpc::encode(subscribe);
					if (childsync::mailbox_append(outbound, bytes.data(), bytes.size()) !=
						childsync::MailboxStatus::OK) {
						return 4;
					}
				}

				if (startup_rpc && !startup_rpc_sent) {
					if (!append_request(outbound, call_id, CMD_GET_OWN_ID, {})) return 4;
					startup_rpc_sent = true;
				}

				const bool startup_heartbeat = startup_rpc_done && !startup_heartbeat_sent;
				const bool periodic_heartbeat =
					!startup_rpc && heartbeat_every != 0 && ticks <= heartbeat_until && (ticks % heartbeat_every) == 0;
				if (startup_heartbeat || periodic_heartbeat) {
					rpc::Frame heartbeat;
					heartbeat.type                   = MT_REQ;
					heartbeat.cmd                    = CMD_HEARTBEAT;
					heartbeat.call_id                = call_id++;
					const std::vector<uint8_t> bytes = rpc::encode(heartbeat);
					if (childsync::mailbox_append(outbound, bytes.data(), bytes.size()) !=
						childsync::MailboxStatus::OK) {
						return 4;
					}
					startup_heartbeat_sent = startup_heartbeat_sent || startup_heartbeat;
				}

				const bool     sent_heartbeat = startup_heartbeat || periodic_heartbeat;
				const unsigned burst_count    = sent_heartbeat && rate_burst >= childsync::MAX_FRAMES_PER_TICK
													? childsync::MAX_FRAMES_PER_TICK - 1
													: rate_burst;
				for (unsigned request = 0; request < burst_count; request++) {
					if (!append_request(outbound, call_id, CMD_LASER_LEFT_GET, {})) return 4;
				}

				if (ticks == 0 && !append_ingress_probe(outbound, ingress_probe)) {
					return 4;
				}

				if (ticks == query_tick) {
					rpc::Frame request;
					request.type                     = MT_REQ;
					request.cmd                      = CMD_LASER_LEFT_GET;
					request.call_id                  = call_id++;
					const std::vector<uint8_t> bytes = rpc::encode(request);
					if (childsync::mailbox_append(outbound, bytes.data(), bytes.size()) !=
						childsync::MailboxStatus::OK) {
						return 4;
					}
				}

				// Deliberately wrong application outputs used by the LTL runtime acceptance tests.
				if (ticks == 2) {
					bool accepted = true;
					if (mutant == "reverse_leg") {
						accepted =
							append_request(outbound, call_id, CMD_BELT_BIG_SET_SPEED, native_payload<int32_t>(-100));
					} else if (mutant == "wrong_laser") {
						accepted = append_request(outbound, call_id, CMD_LASER_RIGHT_SET, {1});
					} else if (mutant == "forbidden_colour") {
						accepted =
							append_request(outbound, call_id, CMD_LED_SET_COLOR, native_payload<uint32_t>(0xDEADBE));
					}
					if (!accepted) return 4;
				}

				ticks++;
			}
		}

		if (certify_quiet) {
			quercus_runahead_certificate_t& certificate = control.certificate[id];
			certificate.now_us                          = local_time_us;
			certificate.next_event_us                   = UINT64_MAX;
			certificate.generation                      = last_seen;
			childsync::store_release(certificate.valid, 1);
		}

		const uint32_t before = childsync::fetch_add_release(control.done_count, 1);
		if (before + 1 == childsync::load_acquire(control.expected)) {
			const uint32_t release_generation = selective_mode ? control.selective[id].generation : last_seen;
			childsync::store_release(control.release, release_generation);
			(void)childsync::futex_wake(control.release, 1);
		}

		if (exit_after != 0 && ticks >= exit_after) {
			return 0;
		}
	}
}
