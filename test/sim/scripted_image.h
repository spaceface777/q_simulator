#pragma once
#include "../../src/sim/runtime.h"

namespace test_app {
	inline std::vector<firmware::ImageBinding> bindings() {
		return {{module::ModuleId{1}, SCRIPTED_IMAGE}, {module::ModuleId{2}, SCRIPTED_IMAGE}};
	}

	class Runtime : public sim::Runtime {
	public:
		explicit Runtime(uint64_t seed) : sim::Runtime(seed, bindings()) {}
	};
} // namespace test_app
