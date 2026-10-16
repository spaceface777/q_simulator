#include "image.h"

#include "../seed/seed.h"
#include "image_os.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace firmware {

	namespace {
		// The host loader validates each new file on its first load, which costs tens of milliseconds
		// on macOS. Copies therefore persist in <root>/<SHA-256 of the image>/<slot>.dll. Within one
		// process each loaded copy needs its own slot, so that the loader does not reuse a loaded
		// image. Separate processes can share a slot.
		constexpr std::size_t KEPT_IMAGES = 8;

		struct Slots {
			std::mutex                                mutex;
			std::map<std::string, std::set<unsigned>> taken; // In use, or never unloaded
		};

		Slots& slots() {
			static Slots instance;
			return instance;
		}

		std::filesystem::path cache_root() {
			if (const char* root = std::getenv("QSIM_IMAGE_CACHE"); root && *root) return root;
			return std::filesystem::temp_directory_path() / "qsim-cache" / "images";
		}

		std::string read_file(const std::filesystem::path& path) {
			std::ifstream input(path, std::ios::binary);
			if (!input) throw std::runtime_error("read: cannot open " + path.string());
			std::ostringstream contents;
			contents << input.rdbuf();
			if (input.bad()) throw std::runtime_error("read: cannot read " + path.string());
			return std::move(contents).str();
		}

		// Hashes each source once per process. Modules commonly share one image.
		std::string digest_of(const std::filesystem::path& source, const std::string& contents) {
			struct Known {
				std::filesystem::file_time_type time;
				std::uintmax_t                  size;
				std::string                     digest;
			};

			static std::mutex                             mutex;
			static std::map<std::filesystem::path, Known> known;
			std::error_code                               error;
			const auto                                    time = std::filesystem::last_write_time(source, error);
			if (error) return seed::sha256_hex(contents);
			std::lock_guard<std::mutex> lock(mutex);
			const auto                  found = known.find(source);
			if (found != known.end() && found->second.time == time && found->second.size == contents.size())
				return found->second.digest;
			auto digest = seed::sha256_hex(contents);
			known.insert_or_assign(source, Known{time, contents.size(), digest});
			return digest;
		}

		bool same_contents(const std::filesystem::path& path, const std::string& contents) {
			std::error_code error;
			if (std::filesystem::file_size(path, error) != contents.size() || error) return false;
			try {
				return read_file(path) == contents;
			} catch (const std::runtime_error&) {
				return false;
			}
		}

		// Writes a complete file and renames it into place, so that no process loads a partial copy.
		void store(const std::filesystem::path& path, const std::string& contents) {
			static std::atomic<uint64_t> sequence{0};
			const auto temporary = std::filesystem::path(path).concat(".tmp-" + std::to_string(os::process_id()) + "-" +
																	  std::to_string(sequence.fetch_add(1)));
			{
				std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
				output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
				output.close();
				if (!output) {
					std::error_code ignored;
					std::filesystem::remove(temporary, ignored);
					throw std::runtime_error("write: cannot write " + temporary.string());
				}
			}
			std::error_code error;
			std::filesystem::rename(temporary, path, error);
			if (error) {
				std::error_code ignored;
				std::filesystem::remove(temporary, ignored);
				// Another process can hold the target open on Windows. Its contents are then the same.
				if (!same_contents(path, contents)) throw std::system_error(error, "rename " + path.string());
			}
		}

		// Removes all but the most recently used images. Removal fails harmlessly for files in use.
		void prune(const std::filesystem::path& root) {
			std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> images;
			std::error_code                                                                error;
			for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
				std::error_code time_error;
				const auto      time = entry.last_write_time(time_error);
				if (entry.is_directory(time_error) && !time_error) images.emplace_back(time, entry.path());
			}
			if (images.size() <= KEPT_IMAGES) return;
			std::sort(images.begin(), images.end(), [](const auto& left, const auto& right) { return left > right; });
			for (auto image = images.begin() + KEPT_IMAGES; image != images.end(); ++image) {
				std::error_code ignored;
				std::filesystem::remove_all(image->second, ignored);
			}
		}

		unsigned acquire_slot(const std::string& digest) {
			auto&                       state = slots();
			std::lock_guard<std::mutex> lock(state.mutex);
			auto&                       taken = state.taken[digest];
			unsigned                    slot  = 0;
			while (taken.contains(slot)) slot++;
			taken.insert(slot);
			return slot;
		}

		void release_slot(const std::string& digest, unsigned slot) {
			auto&                       state = slots();
			std::lock_guard<std::mutex> lock(state.mutex);
			state.taken[digest].erase(slot);
		}
	} // namespace

	LoadedImage::LoadedImage(const std::filesystem::path& source) {
		try {
			const std::string contents = read_file(source);
			digest_                    = digest_of(std::filesystem::absolute(source), contents);
			slot_                      = acquire_slot(digest_);

			const auto      root      = cache_root();
			const auto      directory = root / digest_;
			const bool      created   = std::filesystem::create_directories(directory);
			std::error_code ignored;
			std::filesystem::last_write_time(directory, std::filesystem::file_time_type::clock::now(), ignored);
			if (created) prune(root);

			copy_ = directory / (std::to_string(slot_) + ".dll");
			if (!same_contents(copy_, contents)) store(copy_, contents);
			handle_    = os::open(copy_);
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
		if (!digest_.empty()) {
			// A copy which the loader keeps would bring back its old globals: never reuse its slot.
			if (copy_.empty() || !os::is_loaded(copy_)) release_slot(digest_, slot_);
			digest_.clear();
		}
	}
} // namespace firmware
