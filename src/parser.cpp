#include <vector>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <memory>
#include <unordered_map>

#include "util.cpp"
#include "parser.h"

parser::Attribute parser::parse_attribute(std::string_view line)
{
    const std::vector<std::string_view> parts = util::split(line, " = ");

    if (parts.size() != 2)
    {
        throw new std::runtime_error("Invalid attribute format");
    }

    const std::string_view type = parts[0];
    const std::string_view value = parts[1];

    bool inArray = false;
    bool inString = false;
    size_t last = value.length() - 1;
    for (size_t i = 0; i < value.length(); i++)
    {
        const char c = value[i];
        if (c == '"')
        {
            inString = !inString;
            continue;
        }

        if (inArray && c == ']')
        {
            inArray = false;
            continue;
        }

        if (!inArray && !inString && c == '[')
        {
            inArray = true;
            continue;
        }

        if (!inArray && !inString && c == '#')
        {
            last = i;
            break;
        }
    }

    const parser::Attribute attribute = {
        .name = parts[0],
        .value = parts[1].substr(0, last),
    };

    return attribute;
};

/**
 * Parses the events in the events.toml file.
 */
std::vector<parser::Parsable> parser::parse_events()
{
    const std::unique_ptr<std::ifstream> file = std::make_unique<std::ifstream>("events.toml");

    if (!file->is_open())
    {
        throw new std::runtime_error("Failed to open events.toml");
    }

    int i = 0;
    std::string line;
    while (std::getline(*file, line))
    {
        i++;

        if (line.empty())
        {
            continue;
        }

        if (line[0] == '#')
        {
            continue;
        }

        if (line[0] == '[')
        {
            if (line[line.size() - 1] != ']')
            {
                throw new std::runtime_error("Invalid formatting in line " + i);
            }

            const std::vector<std::string_view> parts = util::split(line, ".");
            if (parts.size() != 2)
            {
                throw new std::runtime_error("Invalid formatting in line " + i);
            }

            const std::string_view type = parts[0].substr(1);
            if (type == "tub")
            {
            }
            else if (type == "plane")
            {
            }
            else
            {
                throw new std::runtime_error("Invalid type in line " + i);
            }
        }
    }

    return std::vector<parser::Parsable>();
}
