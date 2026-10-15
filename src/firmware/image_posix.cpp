#include "image_os.h"

#include <dlfcn.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace firmware::os {
	uint64_t process_id() {
		return getpid();
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
} // namespace firmware::os
