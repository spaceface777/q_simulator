#ifndef LTL_CHECKER_H
#define LTL_CHECKER_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ast.h"

namespace ltl {
	struct TypeEnvironment {
		TypeEnvironment(std::vector<uint16_t> modules, std::optional<uint64_t> scenario_duration_ticks = std::nullopt)
			: modules(std::move(modules)), scenario_duration_ticks(scenario_duration_ticks) {}

		// The immutable module IDs present in the selected layout.
		std::vector<uint16_t> modules;

		// The inclusive final logical tick, when scenario configuration is available.
		std::optional<uint64_t> scenario_duration_ticks;
	};

	// Resolves aliases and annotates every value with its type. Throws std::invalid_argument on
	// the first semantic error, before a specification can be used by a runtime monitor.
	void type_check(File& file, const TypeEnvironment& environment, const std::string& filename = "<ltl>");

} // namespace ltl

#endif // LTL_CHECKER_H
