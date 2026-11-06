// Turns a crash capture into text. Runs after the guarded call has returned, in ordinary context.
#include "crash.h"
#include "crash_symbols.h"

#include <algorithm>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <set>
#include <sstream>
#include <string_view>

namespace firmware::crash {
	namespace {
		std::string hex(uintptr_t value) {
			char text[2 + 2 * sizeof(value) + 1];
			std::snprintf(text, sizeof(text), "0x%" PRIxPTR, value);
			return text;
		}

		const char* signal_name(int number) {
			switch (number) {
			case SIGSEGV: return "SIGSEGV";
#ifdef SIGBUS
			case SIGBUS: return "SIGBUS";
#endif
			case SIGILL: return "SIGILL";
#ifdef SIGTRAP
			case SIGTRAP: return "SIGTRAP";
#endif
			case SIGFPE:  return "SIGFPE";
			case SIGABRT: return "SIGABRT";
#ifdef SIGSYS
			case SIGSYS: return "SIGSYS";
#endif
			default: return "signal";
			}
		}

		struct FaultText {
			std::string summary; // deterministic
			std::string detail;  // may name addresses
		};

		FaultText describe_fault(const Capture& capture) {
			const std::string name = signal_name(capture.signal);
			FaultText         text;

#if defined(__aarch64__)
			const bool     brk       = capture.has_instruction && (capture.instruction & 0xffe0001fu) == 0xd4200000u;
			const unsigned immediate = (capture.instruction >> 5) & 0xffffu;
			// BRK #0x5503 is Clang's trap for the SDK's division-by-zero check.
			if (brk && immediate == 0x5503) {
				text.summary = "integer division by zero";
				text.detail  = "integer division by zero (" + name + ", the compiler's check, BRK #0x5503)";
				return text;
			}
			if (brk) {
				text.summary = "trap instruction";
				text.detail =
					"trap instruction (" + name + ", BRK #" + hex(immediate) + "), for example __builtin_trap()";
				return text;
			}
#elif defined(__x86_64__) || defined(__i386__)
			// Clang's checks trap with UD1 and name the check in the displacement byte:
			// [67] 0F B9 40 <check>, "ud1l <check>(%eax), %eax". Check 3 is the SDK's division by zero,
			// as BRK #0x5503 is on arm64.
			uint64_t ud1 = capture.instruction;
			if ((ud1 & 0xffu) == 0x67u) ud1 >>= 8;
			if (capture.has_instruction && (ud1 & 0xffffffu) == 0x40b90fu) {
				const unsigned check = static_cast<unsigned>(ud1 >> 24) & 0xffu;
				if (check == 3) {
					text.summary = "integer division by zero";
					text.detail  = "integer division by zero (" + name + ", the compiler's check, UD1 #3)";
				} else {
					text.summary = "trap instruction";
					text.detail  = "trap instruction (" + name + ", UD1 #" + hex(check) + "), a compiler check";
				}
				return text;
			}
			// UD2 is __builtin_trap(), and GCC's form of the SDK's division-by-zero check.
			if (capture.has_instruction && (capture.instruction & 0xffffu) == 0x0b0fu) {
				text.summary = "trap instruction";
				text.detail  = "trap instruction (" + name +
							   ", UD2): __builtin_trap(), or a compiler check such as integer division by zero";
				return text;
			}
#endif

#ifdef _WIN32
			// Windows exception codes that say more than the signal they map to.
			switch (static_cast<uint32_t>(capture.code)) {
			case 0xC0000094u:
				text.summary = "integer division by zero";
				text.detail  = text.summary;
				return text;
			case 0xC0000095u:
				text.summary = "integer overflow";
				text.detail  = "integer overflow, for example INT_MIN / -1";
				return text;
			case 0xC00000FDu:
				text.summary = "stack overflow";
				text.detail  = "stack overflow (runaway recursion)";
				return text;
			case 0x80000003u:
				text.summary = "breakpoint instruction";
				text.detail  = "breakpoint instruction (INT3), for example __debugbreak()";
				return text;
			default: break;
			}
#endif

			switch (capture.signal) {
			case SIGSEGV:
#ifdef SIGBUS
			case SIGBUS:
#endif
			{
				text.summary = capture.signal == SIGSEGV ? "segmentation fault (SIGSEGV)" : "bus error (SIGBUS)";
				const bool near_stack =
					capture.stack_pointer &&
					(capture.address > capture.stack_pointer ? capture.address - capture.stack_pointer
															 : capture.stack_pointer - capture.address) < 64 * 1024;
				const bool  jumped = capture.frame_count > 0 && capture.frames[0] == capture.address;
				std::string what;
				if (jumped || capture.access == 3) what = "jumped to " + hex(capture.address) + ", which holds no code";
				else if (capture.access == 2) what = "writing address " + hex(capture.address);
				else if (capture.access == 1) what = "reading address " + hex(capture.address);
				else what = "accessing address " + hex(capture.address);
				if (capture.address < 4096) what += ", a NULL pointer";
				else if (near_stack) {
					text.summary += ", probably a stack overflow";
					what += " beside the stack pointer, probably a stack overflow (runaway recursion)";
				}
				text.detail =
					(capture.signal == SIGSEGV ? "segmentation fault (SIGSEGV) " : "bus error (SIGBUS) ") + what;
				return text;
			}
			case SIGILL:
				text.summary = "illegal instruction (SIGILL)";
				text.detail  = text.summary;
				return text;
			case SIGFPE:
#if defined(FPE_INTDIV)
				if (capture.code == FPE_INTDIV) {
					text.summary = "integer division by zero";
					text.detail  = "integer division by zero (SIGFPE)";
					return text;
				}
				if (capture.code == FPE_INTOVF) {
					text.summary = "integer overflow (SIGFPE)";
					text.detail  = "integer overflow (SIGFPE), for example INT_MIN / -1";
					return text;
				}
#endif
				text.summary = "arithmetic exception (SIGFPE)";
				text.detail  = text.summary;
				return text;
			case SIGABRT:
				text.summary = "abort() in the host C library (SIGABRT)";
				text.detail  = "abort() in the host C library (SIGABRT), for example from assert(), a "
							   "stack-protector check, or a heap-corruption check";
				return text;
			default:
				text.summary = name;
				text.detail  = name + " (signal " + std::to_string(capture.signal) + ")";
				return text;
			}
		}

		std::string description_of(const Capture& capture) {
			return std::string(capture.description.data(),
							   std::find(capture.description.begin(), capture.description.end(), '\0'));
		}

		std::string uptime(uint64_t microseconds) {
			const uint64_t ms = microseconds / 1000;
			char           text[32];
			std::snprintf(text, sizeof(text), "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64 ".%03" PRIu64, ms / 3600000,
						  ms / 60000 % 60, ms / 1000 % 60, ms % 1000);
			return text;
		}

		// Leading frames that only deliver the failure: signal trampolines, abort(), the image's
		// exit path and Zephyr's fatal-error path. The trace starts where the application failed.
		bool delivery_frame(const SymbolizedFrame& frame) {
			static const std::set<std::string_view> names = {"_sigtramp",
															 "__restore_rt",
															 "__pthread_kill",
															 "pthread_kill",
															 "__pthread_kill_implementation",
															 "__pthread_kill_internal",
															 "raise",
															 "gsignal",
															 "abort",
															 "__abort",
															 "__GI_raise",
															 "__GI_abort",
															 "end_application",
															 "qsim_image_fatal",
															 "qsim_image_fatal_error",
															 "qsim_image_fatal_detail",
															 "exit",
															 "_exit",
															 "app_sys_fatal_error_handler",
															 "k_sys_fatal_error_handler",
															 "z_fatal_error",
															 "arch_system_halt",
															 "z_except_reason",
															 "posix_irq_handler"};
			return frame.in_host_executable || names.contains(frame.function);
		}
	} // namespace

	std::string summary(const Capture& capture) {
		switch (capture.cause) {
		case Cause::FAULT: return describe_fault(capture).summary;
		case Cause::HANG:  return "hang: a grant did not return within " + std::to_string(capture.limit_ms) + " ms";
		case Cause::ENDED: return description_of(capture);
		case Cause::NONE:  break;
		}
		return "unknown failure";
	}

	std::string report(const Capture& capture, const ReportContext& context) {
		const std::vector<SymbolizedFrame> frames = symbolize(capture, context.images);
		std::size_t                        first  = 0;
		while (first < frames.size() && delivery_frame(frames[first])) first++;
		if (first == frames.size()) first = 0;

		std::ostringstream out;
		const std::string  module = "Module " + std::to_string(context.module);

		switch (capture.cause) {
		case Cause::FAULT: out << module << " crashed: " << describe_fault(capture).detail << '\n'; break;
		case Cause::HANG:
			out << module << " hung: one grant ran for more than " << capture.limit_ms
				<< " ms without returning (an endless loop, or a wait that never ends)\n";
			break;
		case Cause::ENDED: {
			const std::string what = description_of(capture);
			if (what == "abort()") out << module << " aborted: the application called abort()\n";
			else if (what.rfind("exit(", 0) == 0 || what.rfind("_exit(", 0) == 0)
				out << module << " exited: the application called " << what << '\n';
			else out << module << " crashed: " << what << '\n';
			break;
		}
		case Cause::NONE: out << module << " failed\n"; break;
		}
		out << "  application: " << context.image << '\n';
		if (!context.thread.empty()) out << "  thread:      " << context.thread << '\n';
		out << "  time:        global tick " << context.global_tick << ", module uptime "
			<< uptime(context.module_time_us) << '\n';

		std::set<std::string> without_lines, with_lines;
		out << "Stack trace:\n";
		unsigned   number = 0;
		const auto same   = [](const SymbolizedFrame& a, const SymbolizedFrame& b) {
			return !a.inlined && !b.inlined && b.omitted_before == 0 && a.address == b.address &&
				   a.function == b.function;
		};
		for (std::size_t index = first; index < frames.size(); index++) {
			const SymbolizedFrame& frame = frames[index];
			if (frame.omitted_before) out << "       ... " << frame.omitted_before << " more frames ...\n";
			// Recursion repeats one frame many times. Print it once with a count.
			std::size_t repeats = 0;
			while (index + 1 < frames.size() && same(frame, frames[index + 1])) {
				repeats++;
				index++;
			}
			char prefix[16];
			std::snprintf(prefix, sizeof(prefix), "  %2u: ", number++);
			std::string address = frame.inlined ? std::string() : hex(frame.address);
			address.resize(std::max<std::size_t>(address.size() + 1, 19), ' ');
			std::string function = frame.function.empty() ? "???" : frame.function;
			out << prefix << address << "in " << function;
			if (!frame.file.empty()) {
				if (function.size() < 24) out << std::string(24 - function.size(), ' ');
				out << " at " << frame.file;
				if (frame.line > 0) out << ':' << frame.line;
				else out << " (line unknown: the compiler merged this call with another)";
				if (frame.in_application) with_lines.insert(frame.object);
			} else if (!frame.object.empty()) {
				out << " (" << frame.object << '+' << hex(frame.object_offset) << ')';
				if (frame.in_application) without_lines.insert(frame.object);
			} else {
				out << " (no code at this address)";
			}
			out << '\n';
			if (repeats) out << "       ... the frame above repeats " << repeats << " more times\n";
		}
		if (frames.empty()) out << "  (no frames could be recorded)\n";
		for (const std::string& object : without_lines) {
			if (with_lines.contains(object)) continue;
			out << "  No source lines for " << object << ": its debug information was not found.";
#ifdef __APPLE__
			out << " Rebuild it with qbuild, which writes " << object << ".dSYM beside it.";
#endif
			out << '\n';
		}

		if (!context.recent_log.empty()) {
			out << "Last log output, not yet sent when the module stopped:\n";
			std::istringstream lines(context.recent_log);
			for (std::string line; std::getline(lines, line);) out << "  " << line << '\n';
		}
		out << module << " is stopped. The simulation continues with the other modules.\n";
		return out.str();
	}

} // namespace firmware::crash
