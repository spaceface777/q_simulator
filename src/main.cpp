#include "events/parser.h"
#include "tcp/tcp.h"
#include "tcp/client.h"
#include "packet/packet.h"
#include "module/module.h"
#include <thread>
#include <csignal>
#include "util.h"

static std::thread server_thread;

static void handle_close(int sign)
{
    server::stop();
}

int main(int argc, char const *argv[])
{
    ::signal(SIGINT, handle_close);

    parser::parse_events();

    server_thread = std::thread(server::run, 7777);
    // std::thread client(client::run, 7778);

    const std::filesystem::path path = std::filesystem::path("client");
    module::Module module0(0);
    module0.run(path);

    // module::Module module1(1);
    // module1.run(path);

    // sleep(10);

    // server::stop();
    if (server_thread.joinable())
    {
        server_thread.join();
    }

    // if (client.joinable())
    // {
    //     client.join();
    // }

    return 0;
}
