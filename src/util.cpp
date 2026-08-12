#include "util.h"

#include <algorithm>
#include <cctype>
#include <locale>
#include <vector>

std::vector<std::string_view> util::split(std::string_view sv, std::string_view delimiter) {
	std::vector<std::string_view> tokens;
	size_t                        pos   = 0;
	size_t                        start = 0;
	while ((pos = sv.find(delimiter, start)) != std::string::npos) {
		tokens.push_back(sv.substr(start, pos - start));
		start = pos + delimiter.length();
	}
	tokens.push_back(sv.substr(start));

	return tokens;
}

// gets the index of the last non-space character
std::size_t rtrim_index(std::string_view sv) {
	if (sv.empty()) {
		return size_t(-1);
	}

	std::size_t i = sv.size();
	while (i > 0) {
		const unsigned char ch = static_cast<unsigned char>(sv[i - 1]);
		if (!std::isspace(ch)) {
			return i - 1;
		}
		--i;
	}
	return std::size_t(-1);
}

std::string_view util::ltrim(std::string_view sv) {
	std::size_t i = 0;
	while (i < sv.size()) {
		const unsigned char ch = static_cast<unsigned char>(sv[i]);
		if (!std::isspace(ch)) break;
		++i;
	}
	return sv.substr(i);
}

std::string_view util::rtrim(std::string_view sv) {
	const std::size_t idx = rtrim_index(sv);
	if (idx == size_t(-1)) {
		return std::string_view();
	}
	return sv.substr(0, idx + 1);
}

std::string_view util::trim(std::string_view sv) {
	return rtrim(ltrim(sv));
}

std::optional<uint8_t> util::parse_byte(std::string_view text) {
	try {
		std::size_t     consumed = 0;
		const long long value    = std::stoll(std::string(text), &consumed);
		if (consumed != text.size() || value < 0 || value > std::numeric_limits<uint8_t>::max()) {
			return {};
		}

		return static_cast<uint8_t>(value);
	} catch (const std::invalid_argument&) {
		return {};
	} catch (const std::out_of_range&) {
		return {};
	}
}

std::optional<int> util::parse_int(std::string_view text) {
	try {
		std::size_t     consumed = 0;
		const long long value    = std::stoll(std::string(text), &consumed);
		if (consumed != text.size() || value < std::numeric_limits<int>::min() ||
			value > std::numeric_limits<int>::max()) {
			return {};
		}

		return static_cast<int>(value);
	} catch (const std::invalid_argument&) {
		return {};
	} catch (const std::out_of_range&) {
		return {};
	}
}

std::optional<std::size_t> util::parse_size(std::string_view text) {
	try {
		std::size_t     consumed = 0;
		const long long value    = std::stoll(std::string(text), &consumed);
		if (consumed != text.size() || value < 0) {
			return {};
		}

		return static_cast<std::size_t>(value);
	} catch (const std::invalid_argument&) {
		return {};
	} catch (const std::out_of_range&) {
		return {};
	}
}

std::optional<std::string> util::parse_quoted_string(std::string_view text) {
	if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
		return {};
	}

	return std::string(text.substr(1, text.size() - 2));
}

std::optional<double> util::parse_double(std::string_view text) {
	try {
		std::size_t  consumed = 0;
		const double value    = std::stod(std::string(text), &consumed);
		if (consumed != text.size()) {
			return {};
		}

		return static_cast<double>(value);
	} catch (const std::invalid_argument&) {
		return {};
	} catch (const std::out_of_range&) {
		return {};
	}
}