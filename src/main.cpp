#include <thread>
#include <csignal>
#include <map>
#include <fstream>

#include "event/parser.h"
#include "tcp/server.h"
#include "tcp/client.h"
#include "packet/packet.h"
#include "module/module.h"
#include "util.h"
#include "defs.h"
#include "settings.h"

static std::thread server_thread;

static void handle_close(int sign)
{
    server::stop();
    tub::stop();
}

int main(int argc, char const *argv[])
{
    settings::parse_settings();

    const std::string path = settings::get_setting<std::string>("router_path");
    if (!std::filesystem::exists(path))
    {
        throw std::runtime_error("No zephyr.exe found at router_path in settings.toml");
    }

    ::signal(SIGINT, handle_close);

    std::vector<events::Event> events;
    parser::parse_events(events);

    server_thread = std::thread(server::run, 7777);

    try
    {
        uint8_t module_index = 0;
        while (module_index < settings::get_modules_active())
        {
            std::unique_ptr<module::Module> module = std::make_unique<module::Module>(module_index);
            module->run(path);
            module::add_module(std::move(module));

            while (server::size() < module_index + 1)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            module_index++;
        }
    }
    catch (const std::runtime_error &err)
    {
        if (server_thread.joinable())
        {
            server_thread.join();
        }
        throw err;
    }

    if (server_thread.joinable())
    {
        server_thread.join();
    }
    server::stop();
    tub::stop();

    return 0;
}
