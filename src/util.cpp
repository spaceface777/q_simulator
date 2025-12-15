#include <vector>
#include <string>

namespace util
{

    /**
     * Splits a string at the specified delimiter.
     * @param str The string to split.
     * @param delimiter The delimiter.
     * @return All parts of str split by the delimiter.
     */
    std::vector<std::string_view> split(std::string_view str, std::string_view delimiter)
    {
        std::vector<std::string_view> tokens;
        size_t pos = 0;
        size_t start = 0;
        while ((pos = str.find(delimiter, start)) != std::string::npos)
        {
            tokens.push_back(str.substr(start, pos - start));
            start = pos + delimiter.length();
        }
        tokens.push_back(str.substr(start));

        return tokens;
    }

    /**
     * Returns the final index of the non-space part of a string.
     */
    std::size_t rtrim_index(const std::string_view str)
    {
        const std::size_t length = str.size() - 1;
        std::size_t trailing = length;

        for (; trailing >= 0; trailing--)
        {
            if (str[trailing] != ' ')
            {
                break;
            }
        }

        return trailing;
    }

};