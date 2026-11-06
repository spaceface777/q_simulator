#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Crash containment for application images, which run inside the simulator process. A fault,
// a hang, or an application that ends itself must stop only its own module.
namespace firmware::crash {

	enum class Cause : uint8_t {
		NONE,
		// A CPU exception, or a fatal signal raised by the host C library, for example abort()
		// called by assert() or by a stack-protector check.
		FAULT,
		// One grant ran past the watchdog limit without returning.
		HANG,
		// The application ended itself: exit, _exit, abort, or a Zephyr fatal error.
		ENDED,
	};

	inline constexpr std::size_t MAX_FRAMES = 64;

	// What was known where the application stopped. A signal handler fills this, so it is fixed-size.
	struct Capture {
		Cause     cause         = Cause::NONE;
		int       signal        = 0; // The POSIX signal number, or 0
		int       code          = 0; // si_code, or the Windows exception code
		uintptr_t address       = 0;
		bool      has_address   = false; // A faulting data address is known
		int       access        = 0;     // 1 read, 2 write, 3 execute, 0 unknown
		uintptr_t stack_pointer = 0;
		// A return address that the frame chain may not contain yet: the arm64 link register, or
		// the word at the stack pointer after a call through an invalid pointer on x86.
		uintptr_t caller = 0;
		// The bytes at the program counter, little-endian: 8, or 4 at the end of a mapped page.
		uint64_t instruction     = 0;
		bool     has_instruction = false;
		// frames[0] is the exact program counter. Later entries are return addresses.
		std::array<uintptr_t, MAX_FRAMES> frames{};
		std::size_t                       frame_count = 0;
		// A deep stack keeps its first and last frames. This many were left out before frames[omitted_at].
		std::size_t           omitted    = 0;
		std::size_t           omitted_at = 0;
		uint32_t              limit_ms   = 0; // HANG: the watchdog limit
		std::array<char, 160> description{};  // ENDED: the image's own description
	};

	// Runs call(argument) on this thread and returns true when it returns. If it faults or hangs
	// instead, `capture` says where and this returns false. The code that failed was abandoned
	// mid-execution and must never run again; its library must stay loaded.
	[[nodiscard]] bool run_guarded(void (*call)(void*), void* argument, Capture& capture) noexcept;

	// Records the calling stack. The fatal hook calls this on the application's own stack.
	void capture_here(Capture& capture) noexcept;

	// Milliseconds one guarded call may run before it is treated as hung. QSIM_WATCHDOG_MS sets the
	// initial value, 5000 by default. 0 disables the watchdog. Time while the whole process is
	// stopped, for example in a debugger, does not count.
	[[nodiscard]] uint32_t watchdog_limit_ms() noexcept;
	void                   set_watchdog_limit_ms(uint32_t limit) noexcept;

	// A name for an image's private copy in reports, for example its source path.
	struct ImageName {
		std::string loaded_path;
		std::string display;
	};

	struct ReportContext {
		unsigned               module = 0;
		std::string            image;
		uint64_t               global_tick    = 0;
		uint64_t               module_time_us = 0;
		std::string            thread;
		std::string            recent_log;
		std::vector<ImageName> images;
	};

	// One line without host addresses, for traces and stop reasons. Equal runs produce equal text.
	[[nodiscard]] std::string summary(const Capture& capture);

	// What happened, where, the numbered stack trace, and the newest log text. Symbolizes, so call
	// it only after the guarded call has returned.
	[[nodiscard]] std::string report(const Capture& capture, const ReportContext& context);

} // namespace firmware::crash
