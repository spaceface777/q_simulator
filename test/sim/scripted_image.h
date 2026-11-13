#pragma once
#include "../../src/sim/runtime.h"

namespace test_app {
	std::vector<firmware::ImageBinding> bindings();

	class Runtime : public sim::Runtime {
	public:
		explicit Runtime(uint64_t seed) : sim::Runtime(seed, bindings()) {}
	};
} // namespace test_app
