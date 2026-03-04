#include "event/parser.h"
#include "tcp/server.h"
#include "tcp/client.h"
#include "packet/packet.h"
#include "module/module.h"
#include <thread>
#include <csignal>
#include "util.h"
#include "defs.h"

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

    uint8_t module_index = 0;
    while (module_index < MODULE_COUNT)
    {
        module::Module module(module_index);
        module.run(path);

        while (server::size() < module_index + 1)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        module_index++;
    }

    if (server_thread.joinable())
    {
        server_thread.join();
    }
    server::stop();

    return 0;
}
