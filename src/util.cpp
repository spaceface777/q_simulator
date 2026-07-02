#include <algorithm>
#include <cctype>
#include <locale>
#include <vector>
#include <string_view>
#include <exception>

#include "util.h"

void util::require(bool predicate, std::string_view message)
{
    if (!predicate) {
        throw std::invalid_argument(std::string(message));
    }
}

std::vector<std::string_view> util::split(std::string_view sv, std::string_view delimiter)
{
    std::vector<std::string_view> tokens;
    size_t pos = 0;
    size_t start = 0;
    while ((pos = sv.find(delimiter, start)) != std::string::npos)
    {
        tokens.push_back(sv.substr(start, pos - start));
        start = pos + delimiter.length();
    }
    tokens.push_back(sv.substr(start));

    return tokens;
}

// gets the index of the last non-space character
std::size_t rtrim_index(std::string_view sv)
{
    if (sv.empty())
    {
        return size_t(-1);
    }

    std::size_t i = sv.size();
    while (i > 0)
    {
        const unsigned char ch = static_cast<unsigned char>(sv[i - 1]);
        if (!std::isspace(ch))
        {
            return i - 1;
        }
        --i;
    }
    return std::size_t(-1);
}

std::string_view util::ltrim(std::string_view sv)
{
    std::size_t i = 0;
    while (i < sv.size())
    {
        const unsigned char ch = static_cast<unsigned char>(sv[i]);
        if (!std::isspace(ch))
            break;
        ++i;
    }
    return sv.substr(i);
}

std::string_view util::rtrim(std::string_view sv)
{
    const std::size_t idx = rtrim_index(sv);
    if (idx == size_t(-1))
    {
        return std::string_view();
    }
    return sv.substr(0, idx + 1);
}

std::string_view util::trim(std::string_view sv)
{
    return rtrim(ltrim(sv));
}
