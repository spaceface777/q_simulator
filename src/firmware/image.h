#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace firmware {

	inline constexpr uint32_t IMAGE_ABI_VERSION = 10;

	// The loader needs only the entry point and its version. The backend owns API calls.
	class LoadedImage {
	public:
		explicit LoadedImage(const std::filesystem::path& source);
		~LoadedImage();
		LoadedImage(const LoadedImage&)            = delete;
		LoadedImage& operator=(const LoadedImage&) = delete;

		const void* api() const noexcept { return api_; }

	private:
		void                  cleanup() noexcept;
		std::string           digest_;
		unsigned              slot_ = 0;
		std::filesystem::path copy_;
		void*                 handle_ = nullptr;
		const void*           api_    = nullptr;
	};
} // namespace firmware
