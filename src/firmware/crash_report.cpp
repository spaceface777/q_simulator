#include "crash.h"

#include <csignal>
#include <sstream>

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

	} // namespace

	std::string summary(const Capture& capture) {
		return description(capture.signal);
	}

	std::string report(const Capture& capture, unsigned module, uint64_t tick, uint64_t module_time_us) {
		std::ostringstream text;
		text << "Module " << module << " crashed: " << summary(capture) << " at tick " << tick << " (module time "
			 << module_time_us << " us)\n";
		if (capture.address) text << "Fault address 0x" << std::hex << capture.address << std::dec << '\n';

		for (std::size_t i = 0; i < capture.frame_count; ++i) {
			if (capture.omitted && i == capture.omitted_at) text << "  ... " << capture.omitted << " frames omitted\n";
			text << "  #" << (i + (i >= capture.omitted_at ? capture.omitted : 0)) << " 0x" << std::hex
				 << capture.frames[i] << std::dec << '\n';
		}
		return text.str();
	}
} // namespace firmware::crash
