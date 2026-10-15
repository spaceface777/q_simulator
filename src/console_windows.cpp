#include <cstdio>
#include <io.h>
#include <iostream>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX 1
#include <windows.h>

namespace {
	void connect_stream(FILE* stream, const char* device, const char* mode) {
		if (_fileno(stream) < 0) std::freopen(device, mode, stream);
	}

	struct ParentConsole {
		ParentConsole() {
			if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;

			connect_stream(stdin, "CONIN$", "r");
			connect_stream(stdout, "CONOUT$", "w");
			connect_stream(stderr, "CONOUT$", "w");
			std::cin.clear();
			std::cout.clear();
			std::cerr.clear();
		}
	} parent_console;
} // namespace
