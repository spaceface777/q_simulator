#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <emscripten/emscripten.h>

#include "../../../src/ltl/checker.h"
#include "../../../src/ltl/parser.h"

namespace {

	std::string result;

	std::vector<uint16_t> parse_modules(std::string_view csv) {
		std::vector<uint16_t> modules;
		while (!csv.empty()) {
			const std::size_t      comma = csv.find(',');
			const std::string_view item  = csv.substr(0, comma);
			unsigned               value = 0;
			const auto [end, error]      = std::from_chars(item.data(), item.data() + item.size(), value);
			if (error != std::errc{} || end != item.data() + item.size() || value > UINT16_MAX) {
				throw std::invalid_argument("invalid module id supplied by the editor");
			}
			modules.push_back(static_cast<uint16_t>(value));
			if (comma == std::string_view::npos) break;
			csv.remove_prefix(comma + 1);
		}
		return modules;
	}

	std::string escape_json(std::string_view text) {
		std::string escaped;
		for (const unsigned char character : text) {
			switch (character) {
			case '\\': escaped += "\\\\"; break;
			case '"':  escaped += "\\\""; break;
			case '\n': escaped += "\\n"; break;
			case '\r': escaped += "\\r"; break;
			case '\t': escaped += "\\t"; break;
			default:
				if (character < 0x20) {
					constexpr char hex[] = "0123456789abcdef";
					escaped += "\\u00";
					escaped += hex[character >> 4];
					escaped += hex[character & 0x0f];
				} else {
					escaped += static_cast<char>(character);
				}
			}
		}
		return escaped;
	}

	void diagnostic(std::string_view error) {
		std::size_t                line    = 1;
		std::size_t                column  = 1;
		std::string_view           message = error;
		constexpr std::string_view prefix  = "<document>:";
		if (message.starts_with(prefix)) {
			message.remove_prefix(prefix.size());
			const std::size_t first  = message.find(':');
			const std::size_t second = first == std::string_view::npos ? first : message.find(':', first + 1);
			if (first != std::string_view::npos && second != std::string_view::npos) {
				std::from_chars(message.data(), message.data() + first, line);
				std::from_chars(message.data() + first + 1, message.data() + second, column);
				message.remove_prefix(second + 1);
				if (message.starts_with(' ')) message.remove_prefix(1);
			}
		}
		result = "{\"line\":" + std::to_string(line) + ",\"column\":" + std::to_string(column) + ",\"message\":\"" +
				 escape_json(message) + "\"}";
	}

} // namespace

extern "C" EMSCRIPTEN_KEEPALIVE const char* ltlf_validate(const char* source, const char* module_ids) noexcept {
	try {
		ltl::File file = ltl::parse(source ? source : "", "<document>");
		ltl::type_check(file, ltl::TypeEnvironment{parse_modules(module_ids ? module_ids : "")}, "<document>");
		result.clear();
	} catch (const std::exception& error) {
		diagnostic(error.what());
	} catch (...) {
		diagnostic("unexpected validator failure");
	}
	return result.c_str();
}
