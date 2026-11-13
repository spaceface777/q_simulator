#include "linked_world.h"

#include <stdexcept>

extern "C" {
extern const char             _binary_layout_toml_start[];
extern const char             _binary_layout_toml_end[];
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
		for (std::size_t i = 0; i < modules.size(); ++i) bindings.push_back({modules[i], &__start_qsim_instances[i]});
		return bindings;
	}
} // namespace firmware
