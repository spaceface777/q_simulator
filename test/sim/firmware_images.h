#pragma once
#include "../../src/firmware/backend.h"
#include "../../src/settings.h"
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <vector>

namespace test_applets {
	inline std::string image_path(const std::string& application, bool fixture = false) {
		const auto path =
			std::filesystem::path(fixture ? QSIM_TEST_FIXTURE_DIR : QSIM_TEST_FIRMWARE_DIR) / (application + ".dll");
		if (std::filesystem::is_regular_file(path)) return path.string();
		if (const char* required = std::getenv("QSIM_REQUIRE_FIRMWARE"); required && std::string(required) == "1")
			throw std::runtime_error("Missing firmware image: " + path.string());
		return {};
	}

	inline std::optional<std::vector<firmware::ImageBinding>> bindings(const std::string& application,
																	   bool               fixture = false) {
		const auto path = image_path(application, fixture);
		if (path.empty()) return std::nullopt;
		std::vector<firmware::ImageBinding> bindings;
		for (const auto module : settings::get_active_modules()) bindings.push_back({module, path});
		return bindings;
	}
} // namespace test_applets
