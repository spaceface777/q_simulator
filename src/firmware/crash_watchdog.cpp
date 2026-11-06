#include "crash_watchdog.h"

#include "crash.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace firmware::crash {
	namespace {
		uint32_t initial_limit() {
			const char* text = std::getenv("QSIM_WATCHDOG_MS");
			if (text == nullptr || *text == '\0') return 5000;
			char*               end   = nullptr;
			const unsigned long value = std::strtoul(text, &end, 10);
			return end && *end == '\0' && value <= UINT32_MAX ? static_cast<uint32_t>(value) : 5000;
		}

		std::atomic<uint32_t>& limit() {
			static std::atomic<uint32_t> value{initial_limit()};
			return value;
		}

		std::atomic<uint64_t> calls{0};
		std::atomic<uint64_t> current{0};
		std::once_flag        started;

		void run(void (*interrupt)(uint64_t)) {
			using Clock             = std::chrono::steady_clock;
			constexpr auto interval = std::chrono::milliseconds(50);

			uint64_t          seen  = 0;
			bool              fired = false;
			Clock::duration   running{};
			Clock::time_point last = Clock::now();
			for (;;) {
				std::this_thread::sleep_for(interval);
				const Clock::time_point now = Clock::now();
				// Only time this thread observed counts. A debugger stop or a suspended laptop shows
				// up as one long gap, which must not look like a hang.
				const Clock::duration gap = std::min<Clock::duration>(now - last, 4 * interval);
				last                      = now;
				const uint64_t call       = current.load(std::memory_order_acquire);
				if (call != seen) {
					seen    = call;
					running = Clock::duration{};
					fired   = false;
					continue;
				}
				running += gap;
				const uint32_t limit_ms = limit().load(std::memory_order_relaxed);
				if (call != 0 && !fired && limit_ms != 0 && running >= std::chrono::milliseconds(limit_ms)) {
					fired = true;
					interrupt(call);
				}
			}
		}
	} // namespace

	uint32_t watchdog_limit_ms() noexcept {
		return limit().load(std::memory_order_relaxed);
	}

	void set_watchdog_limit_ms(uint32_t value) noexcept {
		limit().store(value, std::memory_order_relaxed);
	}

	namespace watchdog {
		void start(void (*interrupt)(uint64_t)) {
			std::call_once(started, [interrupt] { std::thread(run, interrupt).detach(); });
		}

		uint64_t next_call() noexcept {
			return calls.fetch_add(1, std::memory_order_relaxed) + 1;
		}

		void watch(uint64_t call) noexcept {
			current.store(call, std::memory_order_release);
		}

		uint64_t watched() noexcept {
			return current.load(std::memory_order_acquire);
		}
	} // namespace watchdog

} // namespace firmware::crash
