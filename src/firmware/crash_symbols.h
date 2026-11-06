#pragma once

#include "crash.h"

#include <cstdint>
#include <string>
#include <vector>

namespace firmware::crash {

	struct SymbolizedFrame {
		uintptr_t   address = 0;
		bool        inlined = false; // Inlined into the next entry, at the same instruction
		std::string function;
		std::string file;
		int         line = 0;
		std::string object; // The library or executable, by its display name
		uintptr_t   object_offset      = 0;
		bool        in_host_executable = false;
		bool        in_application     = false;
		std::size_t omitted_before     = 0; // Frames of a deep stack left out just before this one
	};

	// One entry per physical frame, plus one per inlined call. Never call from a signal handler.
	[[nodiscard]] std::vector<SymbolizedFrame> symbolize(const Capture& capture, const std::vector<ImageName>& images);

	// Tells the symbolizer that libraries were loaded, so that it rereads the loaded objects.
	void images_changed() noexcept;

} // namespace firmware::crash
