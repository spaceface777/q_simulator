#pragma once

#include <cstdint>
#include <filesystem>

namespace firmware::os {
	using Entry = const void* (*)();

	uint64_t process_id();
	void*    open(const std::filesystem::path& path);
	Entry    entry(void* handle);
	void     close(void* handle) noexcept;
} // namespace firmware::os
