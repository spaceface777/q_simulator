#include <thread>
#include <csignal>
#include <map>
#include <fstream>
#include <atomic>

#include "event/parser.h"
#include "tcp/server.h"
#include "tcp/client.h"
#include "packet/packet.h"
#include "module/module.h"
#include "visualization/visualization.h"
#include "util.h"
#include "defs.h"
#include "settings.h"

/**
 * The amount of seconds before a module connection times out.
 */
static constexpr int MODULE_CONNECT_TIMEOUT_SECONDS = 3;

static void handle_close(int sign)
{
    tub::stop();
    server::stop();
    module::stop();
    visualization::stop();

    server::rethrow();
    client::rethrow();
    tub::rethrow();
}

int main(int argc, char const *argv[])
{
    settings::parse_settings();
    std::vector<events::Event> events;
    parser::parse_events(events);

    const std::string path = settings::get_setting<std::string>("router_path");
    if (!std::filesystem::exists(path))
    {
        throw std::runtime_error("No zephyr.exe found at router_path in settings.toml");
    }

    ::signal(SIGINT, handle_close);

    server::run();

    uint8_t module_index = 0;
    while (module_index < settings::get_modules_active())
    {
        std::unique_ptr<module::Module> module = std::make_unique<module::Module>(module_index);
        module->run(path);
        module::add_module(std::move(module));

        int attempts = 0;
        while (server::size() < module_index + 1)
        {
            if (attempts > MODULE_CONNECT_TIMEOUT_SECONDS * 100)
            {
                tub::stop();
                server::stop();
                module::stop();
                visualization::stop();
                throw std::runtime_error("Failed to establish connection with module " + std::to_string(module_index) + " after 2 seconds");
            }

            try
            {
                server::rethrow();
                client::rethrow();
                tub::rethrow();
            }
            catch (const std::exception &ex)
            {
                tub::stop();
                server::stop();
                module::stop();
                visualization::stop();

                server::rethrow();
                client::rethrow();
                tub::rethrow();
                return EXIT_FAILURE;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            attempts++;
            attempts++;
        }

        module_index++;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    visualization::init();
    while (visualization::is_running())
    {
        visualization::update();

        try
        {
            server::rethrow();
            client::rethrow();
            tub::rethrow();
        }
        catch (const std::exception &ex)
        {
            tub::stop();
            server::stop();
            module::stop();
            visualization::stop();

            server::rethrow();
            client::rethrow();
            tub::rethrow();
        }
    }

    return EXIT_SUCCESS;
}
