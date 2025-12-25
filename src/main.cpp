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
    // std::thread client(client::run, 7778);

    const std::filesystem::path path = std::filesystem::path("client");
    module::Module module0(0);
    module0.run(path);

    // module::Module module1(1);
    // module1.run(path);

    if (server.joinable())
    {
        server.join();
    }

    // if (client.joinable())
    // {
    //     client.join();
    // }

    return 0;
}
