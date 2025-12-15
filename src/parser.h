#ifndef PARSER_H
#define PARSER_H

#include <cstdint>
#include <string_view>
#include <vector>

namespace parser
{
    class Parsable
    {
    protected:
        uint8_t _time;
        uint8_t _id;

    public:
        virtual ~Parsable() {}

        /**
         * Returns the arrival time of the object.
         */
        uint8_t time() const
        {
            return _time;
        }

        /**
         * Returns the id of the object.
         */
        uint8_t id() const
        {
            return _id;
        }
    };

    /**
     * Contains the data required for a plane.
     */
    class Plane : public Parsable
    {
        uint8_t _module;
        uint8_t _leaves_in;

    public:
        /**
         * Returns the module that this plane is at.
         */
        uint8_t module() const
        {
            return _module;
        }

        /**
         * The time until leaving for this plane.
         */
        uint8_t leaves_in() const
        {
            return _leaves_in;
        }
    };

    enum LocationType
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
        bool _safe;
        TubData _origin;
        TubData _destination;

    public:
        /**
         * Returns whether this tub is safe.
         */
        bool safe() const
        {
            return _safe;
        }

        /**
         * Returns the origin information for this tub.
         */
        TubData origin() const
        {
            return _origin;
        }

        /**
         * Returns the destination information for this tub.
         */
        TubData destination() const
        {
            return _destination;
        }
    };

    typedef struct
    {
        std::string_view name;
        std::string_view value;
    } Attribute;

    /**
     * Parses a single attribute.
     * @param line The line to parse.
     * @return A struct detailing the values.
     */
    Attribute parse_attribute(std::string_view line);

    /**
     * Parses the events in the events.toml file.
     */
    std::vector<Parsable> parse_events();

}

#endif // PARSER_H