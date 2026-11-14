#include "crash.h"

#include <backtrace.h>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <sstream>

#ifdef __COSMOPOLITAN__
#include <libc/cosmo.h>
#include <unistd.h>
#endif

namespace firmware::crash {
	namespace {
		const char* description(int signal) noexcept {
			if (signal == SIGSEGV) return "segmentation fault (SIGSEGV)";
			if (signal == SIGBUS) return "bus error (SIGBUS)";
			if (signal == SIGFPE) return "arithmetic exception (SIGFPE)";
			if (signal == SIGILL) return "illegal instruction (SIGILL)";
			if (signal == SIGTRAP) return "trap (SIGTRAP)";
			if (signal == SIGABRT) return "abort (SIGABRT)";
			return "fatal signal";
		}

		struct Symbols {
			std::string      file;
			backtrace_state* state;
			std::mutex       mutex;

			Symbols() {
#ifdef __COSMOPOLITAN__
				if (const char* executable = GetProgramExecutableName()) file = std::string(executable) + ".dbg";
#endif
				state = backtrace_create_state(file.empty() ? nullptr : file.c_str(), 0, ignore_error, nullptr);
			}

			static void ignore_error(void*, const char*, int) {}

			struct Frame {
				std::ostringstream& output;
				bool                has_line = false;
			};

			static int line(void* data, uintptr_t, const char* file, int number, const char* function) {
				if (!file || number <= 0) return 0;
				auto& frame = *static_cast<Frame*>(data);
				if (frame.has_line) frame.output << "\n    inlined by ";
				frame.output << (function ? function : "?") << " at " << file << ':' << number;
				frame.has_line = true;
				return 0;
			}

			static void name(void* data, uintptr_t, const char* symbol, uintptr_t, uintptr_t) {
				auto& frame = *static_cast<Frame*>(data);
				frame.output << (symbol ? symbol : "?");
			}
		};
	} // namespace

	std::string summary(const Capture& capture) {
		return description(capture.signal);
	}

	std::string report(const Capture& capture, unsigned module, uint64_t tick, uint64_t module_time_us) {
		std::ostringstream text;
		text << "Module " << module << " crashed: " << summary(capture) << " at tick " << tick << " (module time "
			 << module_time_us << " us)\n";
		if (capture.address) text << "Fault address 0x" << std::hex << capture.address << std::dec << '\n';

		static Symbols  symbols;
		std::lock_guard lock(symbols.mutex);
		bool            any_lines = false;
		for (std::size_t i = 0; i < capture.frame_count; ++i) {
			if (capture.omitted && i == capture.omitted_at) text << "  ... " << capture.omitted << " frames omitted\n";
			const auto address = capture.frames[i];
			text << "  #" << (i + (i >= capture.omitted_at ? capture.omitted : 0)) << " 0x" << std::hex << address
				 << std::dec << ' ';
			Symbols::Frame frame{text};
			if (symbols.state && address) {
				const auto pc = address - (i != 0);
				backtrace_pcinfo(symbols.state, pc, Symbols::line, Symbols::ignore_error, &frame);
				if (!frame.has_line) backtrace_syminfo(symbols.state, pc, Symbols::name, Symbols::ignore_error, &frame);
			}
			any_lines |= frame.has_line;
			text << '\n';
		}
		if (!any_lines) {
			text << "No source lines found";
			if (!symbols.file.empty()) text << " in " << symbols.file;
			text << '\n';
		}
		return text.str();
	}
} // namespace firmware::crash
