#ifndef UTIL_H
#define UTIL_H

#include <vector>
#include <string_view>
#include <chrono>
#include <exception>
#include <stdexcept>
#include <optional>

namespace util
{

    /**
     * Throws exception with message message if !predicate.
     * @throws If !predicate.
     */
    constexpr void require(bool predicate, std::string_view message)
    {
        if (!predicate)
        {
            throw std::invalid_argument(message.data());
        }
    }

    /**
     * Throws out of range exception with message message if !predicate.
     * @throws If !predicate.
     */
    constexpr void require_range(bool predicate, std::string_view message)
    {
        if (!predicate)
        {
            throw std::out_of_range(message.data());
        }
    }

    /**
     * Splits a string at the specified delimiter.
     * @param sv The string to split.
     * @param delimiter The delimiter.
     * @return All parts of str split by the delimiter.
     */
    std::vector<std::string_view> split(std::string_view sv, std::string_view delimiter);

    /**
     * Gets the substring without leading whitespace.
     * @param sv The string view.
     * @return The string view without leading whitespace.
     */
    std::string_view ltrim(std::string_view sv);

    /**
     * Gets the substring without trailing whitespace.
     * @param sv The string view.
     * @return The string view without trailing whitespace.
     */
    std::string_view rtrim(std::string_view sv);

    /**
     * Gets the substring without leading or trailing whitespace.
     * @param sv The string view.
     * @return The string view without leading or trailing whitespace.
     */
    std::string_view trim(std::string_view sv);

    std::optional<uint8_t> parse_byte(std::string_view text);
    std::optional<int> parse_int(std::string_view text);
    std::optional<std::size_t> parse_size(std::string_view text);
    std::optional<std::string> parse_quoted_string(std::string_view text);
    std::optional<double> parse_double(std::string_view text);

}

#endif // UTIL_H