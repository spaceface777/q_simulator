#include "events/parser.h"
#include "tcp/server.h"
#include <thread>

int main(int argc, char const *argv[])
{
    parser::parse_events();

    std::thread server(server::run, 7777);

    if (server.joinable()) {
        server.join();
    }

    return 0;
}
