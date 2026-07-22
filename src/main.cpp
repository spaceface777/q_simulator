#include <thread>
#include <csignal>
#include <map>
#include <fstream>
#include <atomic>
#include <iostream>
#include <list>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>

#include "event/events.h"
#include "tcp/server.h"
#include "tcp/client.h"
#include "message/message.h"
#include "module/module.h"
#include "visualization/visualization.h"
#include "synchronization/virtual_clock.h"
#include "util.h"
#include "defs.h"
#include "settings.h"

/**
 * The amount of seconds before a module connection times out.
 */
static constexpr int MODULE_CONNECT_TIMEOUT_SECONDS = 3;

static void handle_close(int code)
{
    tub::stop();
    server::stop();
    visualization::stop();
    client::stop();

    _exit(code);
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char const *argv[])
{
    ::signal(SIGTERM, handle_close);

    settings::parse_settings("settings.toml");
    std::list<std::unique_ptr<events::Event>> events;
    events::parse_events("events.toml", events);
    server::run();

    uint8_t module_index = 1;
    while (module_index <= settings::get_modules_active())
    {
        std::unique_ptr<module::Module> module = std::make_unique<module::Module>(module_index);
        module->run(settings::get_setting<std::string>("router_path"));
        module::add_module(std::move(module));

        int attempts = 0;
        while (server::size() < module_index)
        {
            if (attempts > MODULE_CONNECT_TIMEOUT_SECONDS * 10)
            {
                tub::stop();
                server::stop();
                visualization::stop();
                client::stop();

                throw std::runtime_error("Failed to establish connection with module " + std::to_string(module_index) + " after " + std::to_string(MODULE_CONNECT_TIMEOUT_SECONDS) + " seconds");
            }

            server::check();
            tub::check();
            client::check();

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            attempts++;
        }

        module_index++;
    }

    while (true)
    {
        const auto modules = module::get_modules();
        bool all = std::all_of(modules.begin(), modules.end(), [](const std::shared_ptr<module::Module> &module)
                               { return module->is_initialization_finished(); });

        if (all)
        {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1000)); // TODO remove this

    // initialize schedule
    for (auto &event : events)
    {
        if (auto *tub_event = dynamic_cast<events::TubEvent *>(event.get()))
        {
            const auto message = message::TubSchedule(tub_event->id(), tub_event->origin().id,
                                                      tub_event->destination().id, tub_event->safe(), tub_event->needs_security());
            for (const auto &module : module::get_modules())
            {
                if (!module)
                {
                    continue;
                }

                module->send(message);
            }
        }
    }

    virtual_clock::VirtualClock vclock = virtual_clock::VirtualClock();
    vclock.set_simulation_speed(settings::get_setting<double>("simulation_speed"));

    visualization::init();
    while (visualization::is_running())
    {
        try
        {
            events::fire_events(vclock, events);
            visualization::update(vclock);
            vclock.tick();

            server::check();
            tub::check();
            client::check();
        }
        catch (std::exception &ex)
        {
            std::cerr << "Shutting down due to failed check: " << ex.what() << std::endl;

            handle_close(-1);
            return EXIT_FAILURE;
        }
    }

    tub::stop();
    tub::join();
    server::stop();
    server::join();
    client::stop();

    return EXIT_SUCCESS;
}
