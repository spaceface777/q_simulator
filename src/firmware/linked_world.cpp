#include "linked_world.h"

#include <algorithm>
#include <stdexcept>

extern "C" {
extern const char             _binary_layout_toml_start[];
extern const char             _binary_layout_toml_end[];
extern const uint8_t          _binary_firmware_digests_start[];
extern const uint8_t          _binary_firmware_digests_end[];
extern const qsim_image_api_t __start_qsim_instances[];
extern const qsim_image_api_t __stop_qsim_instances[];
}

namespace firmware {
	std::string_view linked_layout() {
		return {_binary_layout_toml_start,
				static_cast<std::size_t>(_binary_layout_toml_end - _binary_layout_toml_start)};
	}

	std::vector<ImageBinding> linked_images(std::span<const module::ModuleId> modules) {
		if (__stop_qsim_instances - __start_qsim_instances != static_cast<std::ptrdiff_t>(modules.size()))
			throw std::runtime_error("Linked firmware count does not match the active layout modules");
		std::vector<ImageBinding> bindings;
		constexpr std::size_t     digest_size = 32;
		if (_binary_firmware_digests_end - _binary_firmware_digests_start !=
			static_cast<std::ptrdiff_t>(modules.size() * digest_size))
			throw std::runtime_error("Linked firmware digest count does not match the active layout modules");
		for (std::size_t i = 0; i < modules.size(); ++i) {
			std::array<uint8_t, digest_size> digest;
			std::copy_n(_binary_firmware_digests_start + i * digest_size, digest_size, digest.begin());
			bindings.push_back({modules[i], &__start_qsim_instances[i], digest});
		}
		return bindings;
	}
} // namespace firmware
