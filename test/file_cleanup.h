#pragma once

#include <filesystem>

namespace test_util {
	// Declare before streams and runtimes so their handles close before removal.
	class FileCleanup {
	public:
		explicit FileCleanup(const std::filesystem::path& path) : path_(path) { std::filesystem::remove(path_); }

		~FileCleanup() {
			std::error_code ignored;
			std::filesystem::remove(path_, ignored);
		}

		FileCleanup(const FileCleanup&)            = delete;
		FileCleanup& operator=(const FileCleanup&) = delete;

	private:
		std::filesystem::path path_;
	};
} // namespace test_util
