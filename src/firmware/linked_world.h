#pragma once

#include <string_view>
#include <vector>

#include "inprocess_backend.h"

namespace firmware {
	std::string_view          linked_layout();
	std::vector<ImageBinding> linked_images(std::span<const module::ModuleId> modules);
} // namespace firmware
