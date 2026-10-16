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
#include <cstdio>
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

	void remove_directory(const std::filesystem::path& path) noexcept {
		// Emulation can retain a file mapping after FreeLibrary. POSIX deletion
		// removes its name immediately while existing handles retain the data.
		const auto copy = path / "image.dll";
		HANDLE file = CreateFileW(copy.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
								  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file != INVALID_HANDLE_VALUE) {
			FILE_DISPOSITION_INFO_EX disposition{FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS};
			SetFileInformationByHandle(file, FileDispositionInfoEx, &disposition, sizeof(disposition));
			CloseHandle(file);
		}
		// Also handles failed copies and filesystems without POSIX deletion.
		std::error_code error;
		std::filesystem::remove_all(path, error);
		if (error) std::fprintf(stderr, "Firmware image cleanup failed: %ls (error %d)\n", path.c_str(), error.value());
	}
} // namespace firmware::os
