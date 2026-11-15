#include "sdl.h"

#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>

__static_yoink("zipos");

namespace visualization {
	namespace fs = std::filesystem;
	SdlApi sdl;

	namespace {

		const char* library_name() {
			if (IsWindows()) return "SDL3.dll";
			if (IsXnuSilicon()) return "libSDL3.dylib";
			if (IsLinux()) return "libSDL3.so";
			throw std::runtime_error("GUI support is unavailable on this platform");
		}

		fs::path cache_root() {
			const char* override = std::getenv(IsWindows() ? "LOCALAPPDATA" : "XDG_CACHE_HOME");
			if (override && *override && fs::path(override).is_absolute()) return override;
			if (const char* home = std::getenv("HOME"); home && *home && fs::path(home).is_absolute()) {
				return fs::path(home) / (IsXnu() ? "Library/Caches" : ".cache");
			}
			throw std::runtime_error("Could not find a user cache directory for SDL3");
		}

		std::string read_file(const fs::path& path) {
			std::ifstream input(path, std::ios::binary);
			return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		}

		void cache_file(const fs::path& path, const std::string& bytes) {
			if (read_file(path) == bytes) return;
			fs::create_directories(path.parent_path());
			std::string temporary = path.string() + ".XXXXXX";
			int         fd        = mkstemp(temporary.data());
			if (fd < 0) throw std::system_error(errno, std::generic_category(), "Could not create SDL3 cache file");
			try {
				std::size_t offset = 0;
				while (offset < bytes.size()) {
					const auto written = write(fd, bytes.data() + offset, bytes.size() - offset);
					if (written < 0 && errno == EINTR) continue;
					if (written <= 0)
						throw std::system_error(written < 0 ? errno : EIO, std::generic_category(),
												"Could not write SDL3 cache file");
					offset += written;
				}
				const int result = close(fd);
				fd               = -1;
				if (result != 0)
					throw std::system_error(errno, std::generic_category(), "Could not close SDL3 cache file");
				std::error_code error;
				fs::rename(temporary, path, error);
				if (error && read_file(path) != bytes)
					throw fs::filesystem_error("Could not install SDL3", path, error);
			} catch (...) {
				if (fd >= 0) close(fd);
				unlink(temporary.c_str());
				throw;
			}
			unlink(temporary.c_str());
		}

		fs::path extract(const fs::path& cache, const char* name) {
			const fs::path    embedded = fs::path("/zip/sdl3") / name;
			const std::string bytes    = read_file(embedded);
			if (bytes.empty()) throw std::runtime_error(std::string("This world does not contain ") + name);
			const std::string digest = read_file(embedded.string() + ".sha256");
			if (digest.size() != 64 || digest.find_first_not_of("0123456789abcdef") != std::string::npos)
				throw std::runtime_error(std::string("Invalid embedded digest for ") + name);
			const fs::path path = cache / digest / name;
			cache_file(path, bytes);
			return path;
		}

		template <typename Pointer> void bind(void* library, SdlFunction<Pointer>& function, const char* name) {
			void* symbol = cosmo_dlsym(library, name);
			if (!symbol) throw std::runtime_error(std::string("SDL3 is missing ") + name);
#if defined(__x86_64__)
			if (IsWindows()) {
				function.windows = reinterpret_cast<decltype(function.windows)>(symbol);
				return;
			}
#endif
			function.system_v = reinterpret_cast<Pointer>(cosmo_dltramp(symbol));
			if (!function.system_v) throw std::runtime_error(std::string("Could not bind SDL3 function ") + name);
		}

	} // namespace

	void load_sdl() {
		const char*    name  = library_name();
		const fs::path cache = cache_root() / "qsim" / "sdl3";
		const fs::path path  = extract(cache, name);
		if (IsLinux()) {
			const fs::path helper = extract(cache, "dlopen-helper");
			if (setenv("COSMO_DLOPEN_HELPER", helper.c_str(), 1) != 0)
				throw std::system_error(errno, std::generic_category(), "Could not set the SDL3 loader helper path");
		}
		void* library = cosmo_dlopen(path.c_str(), RTLD_NOW);
		if (!library) {
			const char* error = cosmo_dlerror();
			throw std::runtime_error("Could not load " + path.string() + (error ? ": " + std::string(error) : ""));
		}
		try {
#define QSIM_BIND_SDL(name) bind(library, sdl.name, #name);
			QSIM_SDL_FUNCTIONS(QSIM_BIND_SDL)
#undef QSIM_BIND_SDL
		} catch (...) {
			cosmo_dlclose(library);
			throw;
		}
	}

} // namespace visualization
