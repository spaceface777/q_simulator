// A stand-in Core 1 application, speaking the same contract as
// quercus_sdk/native_sim/lockstep_transport.c, for tests that cannot build a real applet.

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

	void record(const char* path, const rpc::Frame& frame) {
		if (path == nullptr) {
			return;
		}
		std::ofstream file(path, std::ios::app);
		file << static_cast<unsigned>(frame.type) << ":" << static_cast<unsigned>(frame.cmd) << "\n";
	}

	std::vector<uint8_t> float_payload(float value) {
		uint32_t bits = 0;
		std::memcpy(&bits, &value, sizeof(bits));
		return {static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits >> 16),
				static_cast<uint8_t>(bits >> 24)};
	}

	std::vector<uint8_t> uint32_payload(uint32_t value) {
		return {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value >> 16),
				static_cast<uint8_t>(value >> 24)};
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

	const unsigned heartbeat_every = env_number("FAKE_CHILD_HEARTBEAT_EVERY", 50); // 0 disables it.
	const unsigned subscribe_mask  = env_number("FAKE_CHILD_SUBSCRIBE", 0); // Mask to subscribe to on the first tick.
	const unsigned exit_after =
		env_number("FAKE_CHILD_EXIT_AFTER", 0); // Simulates a crash after N ticks. 0 runs forever.
	const unsigned    query_tick       = env_number("FAKE_CHILD_QUERY_TICK", UINT_MAX);
	const bool        reply_to_message = env_number("FAKE_CHILD_REPLY_TO_MESSAGE", 0) != 0 && id == 1;
	const char*       record_path      = ::getenv("FAKE_CHILD_RECORD"); // File to append observed commands to.
	const std::string mutant           = ::getenv("FAKE_CHILD_MUTANT") == nullptr ? "" : ::getenv("FAKE_CHILD_MUTANT");

	uint16_t call_id   = 0x8000;
	unsigned ticks     = 0;
	uint32_t last_seen = 0;

	// Announced only once the mapping is in place, so the parent never ticks an unmapped child.
	if (childsync::load_acquire(control.abi) != QUERCUS_SHM_ABI) {
		std::fprintf(stderr, "fake_child: segment ABI mismatch\n");
		return 2;
	}

	control.ready[id] = 1;

	while (true) {
		while (childsync::load_acquire(control.generation) == last_seen) {
			(void)childsync::futex_wait(control.generation, last_seen, PATIENT);
		}
		last_seen = childsync::load_acquire(control.generation);

		if (control.active[id] != 0) {
			const uint32_t batch_ticks = control.batch_ticks;
			if (batch_ticks == 0 || batch_ticks > childsync::MAX_BATCH_TICKS) return 3;

			for (uint32_t batch_index = 0; batch_index < batch_ticks; batch_index++) {
				bool corrupt          = false;
				bool message_received = false;
				for (const rpc::Frame& frame : take_inbound(pages.to_child[batch_index], corrupt)) {
					// Anything inbound is queued for the application; here it is only recorded so
					// that a test can assert what the child was told.
					record(record_path, frame);
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
					subscribe.payload                = {static_cast<uint8_t>(subscribe_mask)};
					const std::vector<uint8_t> bytes = rpc::encode(subscribe);
					if (childsync::mailbox_append(outbound, bytes.data(), bytes.size()) !=
						childsync::MailboxStatus::OK) {
						return 4;
					}
				}

				if (heartbeat_every != 0 && (ticks % heartbeat_every) == 0) {
					rpc::Frame heartbeat;
					heartbeat.type                   = MT_REQ;
					heartbeat.cmd                    = CMD_HEARTBEAT;
					heartbeat.call_id                = call_id++;
					const std::vector<uint8_t> bytes = rpc::encode(heartbeat);
					if (childsync::mailbox_append(outbound, bytes.data(), bytes.size()) !=
						childsync::MailboxStatus::OK) {
						return 4;
					}
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
						accepted = append_request(outbound, call_id, CMD_BELT_BIG_SET_SPEED, float_payload(-100.0F));
					} else if (mutant == "wrong_laser") {
						accepted = append_request(outbound, call_id, CMD_LASER_RIGHT_SET, {1});
					} else if (mutant == "forbidden_colour") {
						accepted = append_request(outbound, call_id, CMD_LED_SET_COLOR, uint32_payload(0xDEADBE));
					}
					if (!accepted) return 4;
				}

				ticks++;
			}
		}

		const uint32_t before = childsync::fetch_add_release(control.done_count, 1);
		if (before + 1 == childsync::load_acquire(control.expected)) {
			childsync::store_release(control.release, last_seen);
			(void)childsync::futex_wake(control.release, 1);
		}

		if (exit_after != 0 && ticks >= exit_after) {
			return 0;
		}
	}
}
