#ifndef PARSER_H
#define PARSER_H

#include <vector>
#include <string_view>
#include "../event/events.h"

namespace parser
{

    /**
     * Parses the events in the events.toml file.
     * @return All parsed objects.
     */
    std::vector<events::Event> parse_events();

    /**
     * Struct for parsing attributes.
     */
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
    parser::Attribute parse_attribute(std::string_view line);

}

#endif // PARSER_H