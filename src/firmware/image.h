#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace firmware {

	inline constexpr uint32_t IMAGE_ABI_VERSION = 12;

	// The loader needs only the entry point and its version. The backend owns API calls.
	class LoadedImage {
	public:
		explicit LoadedImage(const std::filesystem::path& source);
		~LoadedImage();
		LoadedImage(const LoadedImage&)            = delete;
		LoadedImage& operator=(const LoadedImage&) = delete;

		const void* api() const noexcept { return api_; }

		// The private copy that the loader opened. Crash reports map it back to the source image.
		const std::filesystem::path& loaded_path() const noexcept { return copy_; }

		// Keeps the library loaded for the rest of the process. Code that faulted was abandoned
		// mid-execution: closing its library could run its destructors, or free pages that the
		// host C library still refers to, for example registered handlers or thread state.
		void abandon() noexcept { abandoned_ = true; }

	private:
		void                  cleanup() noexcept;
		std::string           digest_;
		unsigned              slot_ = 0;
		std::filesystem::path copy_;
		void*                 handle_    = nullptr;
		const void*           api_       = nullptr;
		bool                  abandoned_ = false;
	};
} // namespace firmware
