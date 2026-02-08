#ifndef UTIL_H
#define UTIL_H

#include <vector>
#include <string_view>

/**
 * The max amount of modules supported.
 */
#define MAX_MODULES 25

/**
 * Max size for TCP messages.
 */
#define MAX_TCP_BYTES_SIZE 512

/**
 * Max TCP backlog.
 */
#define MAX_TCP_BACKLOG 25

namespace util
{

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

}

#endif // UTIL_H