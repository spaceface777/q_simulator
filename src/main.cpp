#include "events/parser.h"
#include "tcp/tcp.h"
#include "tcp/client.h"
#include "packet/packet.h"
#include "module/module.h"
#include <thread>

int main(int argc, char const *argv[])
{
    parser::parse_events();

    std::thread server(server::run, 7777);
    std::thread client(client::run, 7778);

    if (server.joinable())
    {
        server.join();
    }

    if (client.joinable())
    {
        client.join();
    }

    module::Module module(0);
    module.run(std::filesystem::path("out/build/def/client"));

    return 0;
}
