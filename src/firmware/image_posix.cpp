#include "image_os.h"

#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <cstdio>
#include <linux/fs.h>
#include <memory>
#include <sys/ioctl.h>
#endif

namespace firmware::os {
	uint64_t process_id() {
		return getpid();
	}

	void copy_file(const std::filesystem::path& source, const std::filesystem::path& destination) {
#ifdef __APPLE__
		if (::clonefile(source.c_str(), destination.c_str(), 0) == 0) return;
#elif defined(__linux__)
		using File = std::unique_ptr<FILE, decltype(&std::fclose)>;
		File input(std::fopen(source.c_str(), "rb"), &std::fclose);
		File output(input ? std::fopen(destination.c_str(), "wb") : nullptr, &std::fclose);
		if (input && output && ::ioctl(fileno(output.get()), FICLONE, fileno(input.get())) == 0) return;
#endif
		std::filesystem::copy_file(source, destination, std::filesystem::copy_options::overwrite_existing);
	}

	void* open(const std::filesystem::path& path) {
		void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!handle) throw std::runtime_error(std::string("load library: ") + dlerror());
		return handle;
	}

	Entry entry(void* handle) {
		dlerror();
		auto symbol = reinterpret_cast<Entry>(dlsym(handle, "qsim_image_api"));
		if (const char* error = dlerror()) throw std::runtime_error(std::string("find qsim_image_api: ") + error);
		if (!symbol) throw std::runtime_error("find qsim_image_api: null address");
		return symbol;
	}

	void close(void* handle) noexcept {
		dlclose(handle);
	}

	bool is_loaded(const std::filesystem::path& path) noexcept {
		void* handle = dlopen(path.c_str(), RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
		if (!handle) return false;
		dlclose(handle);
		return true;
	}
} // namespace firmware::os
