#include "image.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace firmware {

	LoadedImage::LoadedImage(const std::filesystem::path& source) {
		try {
			std::string pattern = (std::filesystem::temp_directory_path() / "qsim-image-XXXXXX").string();
			if (!::mkdtemp(pattern.data())) throw std::system_error(errno, std::generic_category(), "mkdtemp");
			directory_           = pattern;
			const auto      copy = directory_ / "image.dll";
			std::error_code copy_error;
			std::filesystem::copy_file(source, copy, copy_error);
			if (copy_error) throw std::system_error(copy_error, "copy_file");
			handle_ = ::dlopen(copy.c_str(), RTLD_NOW | RTLD_LOCAL);
			if (!handle_) throw std::runtime_error(std::string("dlopen: ") + ::dlerror());
			::dlerror();
			auto entry = reinterpret_cast<const void* (*)()>(::dlsym(handle_, "qsim_image_api"));
			if (const char* error = ::dlerror()) throw std::runtime_error(std::string("dlsym: ") + error);
			if (!entry) throw std::runtime_error("qsim_image_api has a null address");
			api_ = entry();
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
			::dlclose(handle_);
			handle_ = nullptr;
		}
		if (!directory_.empty()) {
			std::error_code ignored;
			std::filesystem::remove_all(directory_, ignored);
		}
	}
} // namespace firmware
