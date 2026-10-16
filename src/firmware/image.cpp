#include "image.h"

#include "image_os.h"

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace firmware {

	LoadedImage::LoadedImage(const std::filesystem::path& source) {
		try {
			static std::atomic<uint64_t> sequence{0};
			const auto                   temporary = std::filesystem::temp_directory_path();
			for (;;) {
				const auto candidate = temporary / ("qsim-image-" + std::to_string(os::process_id()) + "-" +
													std::to_string(sequence.fetch_add(1)));
				if (std::filesystem::create_directory(candidate)) {
					directory_ = candidate;
					break;
				}
			}

			const auto      copy = directory_ / "image.dll";
			std::error_code copy_error;
			std::filesystem::copy_file(source, copy, copy_error);
			if (copy_error) throw std::system_error(copy_error, "copy_file");
			handle_    = os::open(copy);
			auto entry = os::entry(handle_);
			api_       = entry();
			if (!api_) throw std::runtime_error("qsim_image_api returned null");
			uint32_t version;
			std::memcpy(&version, api_, sizeof(version));
			if (version != IMAGE_ABI_VERSION)
				throw std::runtime_error("ABI version " + std::to_string(version) + "; expected " +
										 std::to_string(IMAGE_ABI_VERSION));
		} catch (const std::exception& error) {
			cleanup();
			throw std::runtime_error("Firmware image '" + source.string() + "': " + error.what());
		}
	}

	LoadedImage::~LoadedImage() {
		cleanup();
	}

	void LoadedImage::cleanup() noexcept {
		if (handle_) {
			os::close(handle_);
			handle_ = nullptr;
		}
		if (!directory_.empty()) {
			os::remove_directory(directory_);
		}
	}
} // namespace firmware
