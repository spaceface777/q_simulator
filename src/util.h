#ifndef UTIL_H
#define UTIL_H

#include <chrono>
#include <exception>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace util {

	// Throws std::invalid_argument with message if !predicate.
	constexpr void require(bool predicate, std::string_view message) {
		if (!predicate) {
			throw std::invalid_argument(message.data());
		}
	}

	// Throws std::out_of_range with message if !predicate.
	constexpr void require_range(bool predicate, std::string_view message) {
		if (!predicate) {
			throw std::out_of_range(message.data());
		}
	}

	std::vector<std::string_view> split(std::string_view sv, std::string_view delimiter);

	std::string_view ltrim(std::string_view sv);

	std::string_view rtrim(std::string_view sv);

	std::string_view trim(std::string_view sv);

	std::optional<uint8_t>     parse_byte(std::string_view text);
	std::optional<int>         parse_int(std::string_view text);
	std::optional<std::size_t> parse_size(std::string_view text);
	std::optional<std::string> parse_quoted_string(std::string_view text);
	std::optional<double>      parse_double(std::string_view text);

} // namespace util

#endif // UTIL_H