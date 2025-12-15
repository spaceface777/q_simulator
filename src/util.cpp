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
    std::vector<std::string_view> split(const std::string_view str, const std::string_view delimiter)
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

};