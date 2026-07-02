#ifndef PARSER_H
#define PARSER_H

#include <list>
#include <string_view>
#include "../event/events.h"

namespace parser
{

    /**
     * Parses the events in the events.toml file.
     * @param events Vector to populate with parsed events.
     */
    void parse_events(std::list<events::Event> &events);

}

#endif // PARSER_H