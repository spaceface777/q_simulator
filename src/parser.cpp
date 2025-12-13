#include <vector>
#include <cstdint>
#include <iostream>
#include <fstream>
#include <memory>

#include "util.cpp"

class Parsable
{
protected:
    uint8_t time;
    uint8_t id;

public:
    virtual ~Parsable() {}

    /**
     * Returns the arrival time of the object.
     */
    uint8_t time()
    {
        return time;
    }

    /**
     * Returns the id of the object.
     */
    uint8_t id()
    {
        return id;
    }
};

/**
 * Contains the data required for a plane.
 */
class Plane : public Parsable
{
    uint8_t module;
    uint8_t leaves_in;

public:
    /**
     * Returns the module that this plane is at.
     */
    uint8_t module()
    {
        return module;
    }

    /**
     * The time until leaving for this plane.
     */
    uint8_t leaves_in()
    {
        return leaves_in;
    }
};

typedef enum LocationType
{
    MODULE,
    PLANE
};

typedef struct
{
    LocationType type;
    uint8_t id;
} TubData;

/**
 * Contains the data required for a tub.
 */
class Tub : public Parsable
{
    bool safe;
    TubData origin;
    TubData destination;

public:
    /**
     * Returns whether this tub is safe.
     */
    bool safe()
    {
        return safe;
    }

    /**
     * Returns the origin information for this tub.
     */
    TubData origin()
    {
        return origin;
    }

    /**
     * Returns the destination information for this tub.
     */
    TubData destination()
    {
        return destination;
    }
};

Parsable parse_attribute(std::string line)
{
    const std::vector<std::string> parts = util::split(line, " = ");

    if (parts.size() != 2)
    {
        throw new std::runtime_error("Invalid attribute format");
    }

    const std::string type = parts[0];
    const std::string value = parts[1];

    // todo!
};

/**
 * Parses the events in the events.toml file.
 */
std::vector<Parsable>
parse_events()
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

            const std::vector<std::string> parts = util::split(line, ".");
            if (parts.size() != 2)
            {
                throw new std::runtime_error("Invalid formatting in line " + i);
            }

            const std::string type = parts[0].substr(1);
            if (type == "tub")
            {
            }
            else if (type == "plane")
            {
            }
            else
            {
                throw new std::runtime_error("Invalid type " + type);
            }
        }
    }

    return std::vector<Parsable>();
}
