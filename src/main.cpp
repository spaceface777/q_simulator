#include "events/parser.h"
#include "tcp/server.h"
#include "purple/purple.h"
#include <thread>

int main(int argc, char const *argv[])
{
    parser::parse_events();

    std::thread server(server::run, 7777);

    if (server.joinable())
    {
        server.join();
    }

    purple::Message msg = purple::Update(1, "2", 3);

    return 0;
}
