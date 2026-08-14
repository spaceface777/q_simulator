#ifndef LTL_PARSER_H
#define LTL_PARSER_H

#include <cstdint>
#include <string>

#include "ast.h"

namespace ltl {

	// Parses syntax only. Call type_check() before consuming the AST as a compiled specification.
	[[nodiscard]] File parse(const std::string& source, const std::string& filename = "<ltl>");

	// Uses the same unit ordering and exact 1 ms conversion as property scopes.
	[[nodiscard]] uint64_t parse_duration(const std::string& source, const std::string& filename = "<duration>");

} // namespace ltl

#endif // LTL_PARSER_H
