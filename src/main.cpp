#include "events/parser.h"
#include "tcp/server.h"
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

    const std::filesystem::path path = std::filesystem::path("/home/efy/Projects/airport-router/build/zephyr/zephyr.exe");
    module::Module module0(0);
    module0.run(path);

    while (server::size() < 1)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    module::Module module1(1);
    module1.run(path);

    while (server::size() < 2)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // server::stop();
    if (server_thread.joinable())
    {
        server_thread.join();
    }

    return 0;
}
