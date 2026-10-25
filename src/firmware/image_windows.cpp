#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000002
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX 1

#include "image_os.h"

#include <bit>
#include <system_error>
#include <windows.h>

namespace firmware::os {
	uint64_t process_id() {
		return GetCurrentProcessId();
	}

	void copy_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
		std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
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

	bool is_loaded(const std::filesystem::path& path) noexcept {
		return GetModuleHandleW(path.c_str()) != nullptr;
	}
} // namespace firmware::os
