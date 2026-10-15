#include "image_os.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX 1
#include <bit>
#include <system_error>
#include <windows.h>

namespace firmware::os {
	uint64_t process_id() {
		return GetCurrentProcessId();
	}

	void* open(const std::filesystem::path& path) {
		void* handle = LoadLibraryW(path.c_str());
		if (!handle) throw std::system_error(GetLastError(), std::system_category(), "load library");
		return handle;
	}

	Entry entry(void* handle) {
		auto symbol = std::bit_cast<Entry>(GetProcAddress(static_cast<HMODULE>(handle), "qsim_image_api"));
		if (!symbol) throw std::system_error(GetLastError(), std::system_category(), "find qsim_image_api");
		return symbol;
	}

	void close(void* handle) noexcept {
		FreeLibrary(static_cast<HMODULE>(handle));
	}
} // namespace firmware::os
