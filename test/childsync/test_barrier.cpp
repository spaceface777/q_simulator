#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <climits>
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "../../src/childsync/barrier.h"

namespace {

	constexpr std::chrono::microseconds PATIENT{5000000};

	constexpr uint32_t TICKS = 20;

	// The child half of the protocol in src/sim/runtime.cpp, standing in for a real module. A child
	// answers every generation, whether or not it was granted the tick.
	void run_child(childsync::Control& control, childsync::Segment& segment, uint8_t id, uint32_t generations) {
		uint32_t last_seen = 0;

		for (uint32_t seen = 0; seen < generations; seen++) {
			while (childsync::load_acquire(control.generation) == last_seen) {
				(void)childsync::futex_wait(control.generation, last_seen, PATIENT);
			}
			last_seen = childsync::load_acquire(control.generation);

			if (control.active_mask[id] != 0) {
				// Prove the grant fields and the inbound mailbox landed before the wake.
				std::size_t    length  = 0;
				const uint8_t* inbound = childsync::mailbox_read(segment.to_child[0], 0, length);
				if (inbound == nullptr || length != 1) {
					::_exit(2);
				}

				childsync::mailbox_clear(segment.to_parent[0]);
				const uint8_t reply = id;
				if (childsync::mailbox_append(segment.to_parent[0], &reply, 1) != childsync::MailboxStatus::OK) {
					::_exit(3);
				}
			}

			const uint32_t before = childsync::fetch_add_release(control.done_count, 1);
			if (before + 1 == childsync::load_acquire(control.expected)) {
				childsync::store_release(control.release, last_seen);
				(void)childsync::futex_wake(control.release, 1);
			}
		}

		::_exit(0);
	}

	void await_barrier(childsync::Control& control, uint32_t generation) {
		while (true) {
			const uint32_t released = childsync::load_acquire(control.release);
			if (released == generation) {
				return;
			}
			(void)childsync::futex_wait(control.release, released, PATIENT);
		}
	}

	TEST(Barrier, RendezvousesEveryChildOnEveryTick) {
		constexpr uint8_t CHILDREN = 4;

		childsync::SharedControl shared;
		childsync::Control&      control = shared.get();
		control.expected                 = CHILDREN;

		std::vector<childsync::ChildSegment> segments(CHILDREN);

		std::vector<pid_t> pids;
		for (uint8_t id = 0; id < CHILDREN; id++) {
			const pid_t pid = ::fork();
			ASSERT_GE(pid, 0);
			if (pid == 0) {
				run_child(control, segments[id].get(), id, TICKS);
			}
			pids.push_back(pid);
		}

		for (uint32_t tick = 1; tick <= TICKS; tick++) {
			for (uint8_t id = 0; id < CHILDREN; id++) {
				control.active_mask[id] = 1;
				childsync::mailbox_clear(segments[id].get().to_child[0]);
				const uint8_t stamp = static_cast<uint8_t>(tick);
				ASSERT_EQ(childsync::mailbox_append(segments[id].get().to_child[0], &stamp, 1),
						  childsync::MailboxStatus::OK);
			}

			childsync::store_relaxed(control.done_count, 0);
			childsync::store_release(control.generation, tick);
			ASSERT_GE(childsync::futex_wake(control.generation, INT_MAX), 0);

			await_barrier(control, tick);

			// Every child's work is visible because its done_count increment released it.
			for (uint8_t id = 0; id < CHILDREN; id++) {
				std::size_t    length = 0;
				const uint8_t* reply  = childsync::mailbox_read(segments[id].get().to_parent[0], 0, length);
				ASSERT_NE(reply, nullptr) << "child " << static_cast<unsigned>(id) << " tick " << tick;
				ASSERT_EQ(length, 1u);
				EXPECT_EQ(reply[0], id);
			}
		}

		for (const pid_t pid : pids) {
			int status = 0;
			ASSERT_EQ(::waitpid(pid, &status, 0), pid);
			EXPECT_TRUE(WIFEXITED(status));
			EXPECT_EQ(WEXITSTATUS(status), 0);
		}
	}

	TEST(Barrier, SelectiveWakeSequenceIgnoresUnrelatedBankNotifications) {
		constexpr uint32_t            CHILDREN = 40;
		const std::array<uint32_t, 4> selected{0, 7, 32, 39};
		childsync::SharedControl      shared;
		childsync::Control&           control = shared.get();
		std::vector<pid_t>            pids;
		for (uint32_t index = 0; index < CHILDREN; index++) {
			control.scheduler_index[index] = static_cast<uint8_t>(index);
			const pid_t pid                = ::fork();
			ASSERT_GE(pid, 0);
			if (pid == 0) {
				const uint32_t bank = index / 32U;
				const uint32_t mask = UINT32_C(1) << (index % 32U);
				while (childsync::load_acquire(control.wake_seq[index]) == 0) {
					const uint32_t observed = childsync::load_acquire(control.futex_bank[bank]);
					if (childsync::load_acquire(control.wake_seq[index]) != 0) break;
					(void)childsync::futex_wait_bitset(control.futex_bank[bank], observed, mask, PATIENT);
				}
				(void)childsync::fetch_add_release(control.done_count, 1);
				::_exit(0);
			}
			pids.push_back(pid);
		}

		std::array<uint32_t, 2> masks{};
		for (const uint32_t index : selected) {
			childsync::store_release(control.wake_seq[index], 1);
			masks[index / 32U] |= UINT32_C(1) << (index % 32U);
		}
		for (uint32_t bank = 0; bank < masks.size(); bank++) {
			childsync::store_release(control.futex_bank[bank], 1);
			ASSERT_GE(childsync::futex_wake_bitset(control.futex_bank[bank], INT_MAX, masks[bank]), 0);
		}
		for (uint32_t attempt = 0; attempt < 1000 && childsync::load_acquire(control.done_count) != selected.size();
			 attempt++)
			::usleep(1000);
		EXPECT_EQ(childsync::load_acquire(control.done_count), selected.size());

		for (uint32_t index = 0; index < CHILDREN; index++) {
			if (std::ranges::find(selected, index) == selected.end()) (void)::kill(pids[index], SIGKILL);
			int status = 0;
			ASSERT_EQ(::waitpid(pids[index], &status, 0), pids[index]);
		}
	}

	// A paused child still answers the barrier; it is withheld the tick, not the rendezvous.
	TEST(Barrier, WithholdsTheTickFromAPausedChildWithoutStalling) {
		constexpr uint32_t GENERATIONS = 30;

		childsync::SharedControl shared;
		childsync::Control&      control = shared.get();
		control.expected                 = 1;

		childsync::ChildSegment segment;

		const pid_t pid = ::fork();
		ASSERT_GE(pid, 0);
		if (pid == 0) {
			run_child(control, segment.get(), 0, GENERATIONS);
		}

		uint32_t granted = 0;
		for (uint32_t generation = 1; generation <= GENERATIONS; generation++) {
			const bool paused      = (generation % 3) == 0;
			control.active_mask[0] = paused ? 0 : 1;

			if (!paused) {
				granted++;
				childsync::mailbox_clear(segment.get().to_child[0]);
				const uint8_t stamp = static_cast<uint8_t>(granted);
				ASSERT_EQ(childsync::mailbox_append(segment.get().to_child[0], &stamp, 1),
						  childsync::MailboxStatus::OK);
			}

			childsync::store_relaxed(control.done_count, 0);
			childsync::store_release(control.generation, generation);
			ASSERT_GE(childsync::futex_wake(control.generation, INT_MAX), 0);

			await_barrier(control, generation);
		}

		EXPECT_EQ(granted, 20u);

		int status = 0;
		ASSERT_EQ(::waitpid(pid, &status, 0), pid);
		EXPECT_TRUE(WIFEXITED(status));
		EXPECT_EQ(WEXITSTATUS(status), 0);
	}

} // namespace
