#include <vector>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <memory>
#include <unordered_map>
#include <string>

#include "util.h"
#include "parser.h"

parser::Attribute parser::parse_attribute(std::string_view line)
{
    const std::vector<std::string_view> parts = util::split(line, "=");

    if (parts.size() != 2)
    {
        throw new std::runtime_error("Invalid attribute format");
    }

    const std::string_view type = util::trim(parts[0]);
    const std::string_view value = util::trim(parts[1]);

    // find last character that is not a comment
    bool in_array = false;
    bool in_string = false;
    std::size_t last = value.length();
    for (std::size_t i = 0; i < value.length(); i++)
    {
        const char c = value[i];
        if (c == '"' && (i == 0 || value[i - 1] != '\\'))
        {
            in_string = !in_string;
            continue;
        }

        if (in_array && c == ']')
        {
            in_array = false;
            continue;
        }

        if (!in_array && !in_string && c == '[')
        {
            in_array = true;
            continue;
        }

        if (!in_array && !in_string && c == '#')
        {
            last = i;
            break;
        }
    }

    // strip comment
    const std::string_view stripped = value.substr(0, last);
    const parser::Attribute attribute = {
        .name = type,
        .value = util::rtrim(stripped),
    };

    return attribute;
};

enum ParsableType
{
    NONE,
    TUB,
    PLANE
};

struct State
{
    /**
     * The pointer to the file being read.
     */
    std::ifstream file;

    /**
     * The current line.
     */
    std::size_t idx;

    /**
     * The type being currently parsed.
     */
    ParsableType type;
};

/**
 * Parses an int from sv.
 * @param sv The string to parse as an int.
 * @param name The name of the attribute.
 * @return The parsed integer, or throws if invalid bounds.
 * @throws std::invalid_argument for out of bounds values.
 */
int parse_int(std::string_view sv, std::string_view name)
{
    int value = std::stoi(std::string(sv));
    if (value < 0 || value > UINT8_MAX)
    {
        throw std::invalid_argument("Invalid value for " + std::string(name) + ": " + std::to_string(value));
    }
    return value;
}

/**
 * Parses an bool from sv.
 * @param sv The string to parse as an bool.
 * @param name The name of the attribute.
 * @return The parsed bool, or throws if invalid bounds.
 * @throws std::invalid_argument for out of bounds values.
 */
bool parse_bool(std::string_view sv, std::string_view name) 
{
    if (sv == "true") {
        return true;
    } else if (sv == "false") {
        return false;
    }

    throw std::invalid_argument("Invalid value for " + std::string(name) + ": " + std::string(sv));
}

/**
 * Parses the attributes of a tub.
 * @param state The state of the parsing machine.
 * @return The parsed tub, or throws for invalid attributes or values.
 */
events::Tub parse_tub(State &state)
{
    int time = -1, id = -1, safe = -1;
    events::TubData origin{events::LocationType::MODULE, -1};
    events::TubData destination{events::LocationType::MODULE, -1};

    std::string line;
    while (std::getline(state.file, line))
    {
        const std::string_view view = util::ltrim(line);

        if (view.empty())
        {
            state.idx++;
            continue;
        }

        if (view[0] == '#')
        {
            state.idx++;
            continue;
        }

        if (view[0] == '[')
        {
            break;
        }

        // parse attributes
        const parser::Attribute attribute = parser::parse_attribute(line);
        if (attribute.name == "id")
        {
            id = parse_int(attribute.value, "id");
        }
        else if (attribute.name == "time")
        {
            time = parse_int(attribute.value, "name");
        }
        else if (attribute.name == "safe")
        {
            safe = parse_bool(attribute.value, "safe");
        }
        else if (attribute.name == "origin" || attribute.name == "destination")
        {
            const std::vector<std::string_view> parts = util::split(attribute.value, ",");
            const std::string_view location_sv = util::trim(parts[0]).substr(1);
            const std::string_view raw_id = parts[1];

            events::TubData data;
            data.id = std::stoi(std::string(raw_id.substr(0, raw_id.size())));
            if (location_sv == "\"module\"")
            {
                data.type = events::LocationType::MODULE;
            }
            else if (location_sv == "\"plane\"")
            {
                data.type = events::LocationType::PLANE;
            }
            else
            {
                throw std::invalid_argument("Invalid location: " + std::string(location_sv));
            }

            if (attribute.name == "origin")
            {
                origin = data;
            }
            else if (attribute.name == "destination")
            {
                destination = data;
            }
        }
        else
        {
            throw std::invalid_argument("Invalid attribute: " + std::string(attribute.name));
        }

        state.idx++;
    }

    if (time == -1 || id == -1 || safe == -1 || origin.id == -1 || destination.id == -1)
    {
        throw std::invalid_argument("Missing attributes");
    }

    return events::Tub(time, id, safe, origin, destination);
}

/**
 * Parses the attributes of a plane.
 * @param state The state of the parsing machine.
 * @return The parsed plane, or throws for invalid attributes or values.
 */
events::Plane parse_plane(State &state)
{
    int time = -1, id = -1, module = -1, leaves_at = -1;

    std::string line;
    while (std::getline(state.file, line))
    {
        const std::string_view view = util::ltrim(line);

        if (view.empty())
        {
            state.idx++;
            continue;
        }

        if (view[0] == '#')
        {
            state.idx++;
            continue;
        }

        if (view[0] == '[')
        {
            break;
        }

        // parse attributes
        const parser::Attribute attribute = parser::parse_attribute(line);
        if (attribute.name == "id")
        {
            id = parse_int(attribute.value, "id");
        }
        else if (attribute.name == "time")
        {
            time = parse_int(attribute.value, "name");
        }
        else if (attribute.name == "module")
        {
            module = parse_int(attribute.value, "module");
        }
        else if (attribute.name == "leaves_in")
        {
            leaves_at = parse_int(attribute.value, "leaves_in");
        }
        else
        {
            throw std::invalid_argument("Invalid attribute: " + std::string(attribute.name));
        }

        state.idx++;
    }

    if (time == -1 || id == -1 || module == -1 || leaves_at == -1)
    {
        throw std::invalid_argument("Missing attributes");
    }

    return events::Plane(time, id, module, leaves_at);
}

/**
 * Parses the events in the events.toml file.
 * @return A list of all events.
 */
std::vector<events::Event> parser::parse_events()
{
    State state;
    state.file = std::ifstream("events.toml");
    state.idx = 0;
    state.type = ParsableType::NONE;

    std::vector<events::Event> objects = std::vector<events::Event>();
    objects.reserve(16);

    if (!state.file.is_open())
    {
        throw new std::runtime_error("Failed to open events.toml");
    }

    std::string line;
    while (std::getline(state.file, line))
    {
        if (line.empty())
        {
            state.idx++;
            continue;
        }

        if (line[0] == '#')
        {
            state.idx++;
            continue;
        }

        if (line[0] == '[')
        {
            if (line.back() != ']')
            {
                throw std::runtime_error("Invalid formatting in line " + std::to_string(state.idx));
            }

            const std::vector<std::string_view> parts = util::split(line, ".");
            if (parts.size() != 2)
            {
                throw std::runtime_error("Invalid formatting in line " + std::to_string(state.idx));
            }

            const std::string_view type = parts[0].substr(1);
            if (type == "tub")
            {
                state.type = ParsableType::TUB;
                objects.push_back(parse_tub(state));
                state.type = ParsableType::NONE;
            }
            else if (type == "plane")
            {
                state.type = ParsableType::PLANE;
                objects.push_back(parse_plane(state));
                state.type = ParsableType::NONE;
            }
            else
            {
                throw std::runtime_error("Invalid type in line " + std::to_string(state.idx));
            }
        }

        state.idx++;
    }

    state.file.close();
    return objects;
}
