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
#include <chrono>
#include <string_view>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/utsname.h>

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
 * Target FPS.
 */
static constexpr double FPS_TARGET = 60;

/**
 * The amount of seconds before a module connection times out.
 */
static constexpr std::chrono::seconds MODULE_CONNECT_TIMEOUT = std::chrono::seconds(3);

constexpr std::string_view get_target_os()
{
#if defined(__APPLE__) && defined(__MACH__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#else
    return "Unknown OS";
#endif
}

constexpr std::string_view get_target_arch()
{
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "ARM64";
#elif defined(__arm__) || defined(_M_ARM)
    return "ARM";
#else
    return "Unknown Arch";
#endif
}

static bool using_visualization = true;
static bool is_running = true;

/**
 * Handles closing interrupts.
 * @param code The interrupt.
 */
static void handle_close(int code)
{
    tub::stop();
    tub::join();
    server::stop();
    server::join();
    client::stop();
    if (using_visualization)
        visualization::stop();

    is_running = false;

    ::_exit(code);
}

/**
 * Initializes all module instances and starts the router processes.
 */
static void init_modules()
{
    for (std::size_t id = 1; id < MAX_MODULES; id++)
    {
        if (!settings::get_module_settings(module::ModuleId(id)).active)
            continue;

        std::unique_ptr<module::Module> module = std::make_unique<module::Module>(id);
        module->run(settings::get_setting<std::string>("router_path"));
        module::add_module(std::move(module));

        int attempts = 0;
        while (server::size() < id)
        {
            if (attempts > std::chrono::duration_cast<std::chrono::seconds>(MODULE_CONNECT_TIMEOUT).count() * 10)
            {
                tub::stop();
                server::stop();
                client::stop();
                if (using_visualization)
                    visualization::stop();

                throw std::runtime_error("Failed to establish connection with module " + std::to_string(id) + " after " + std::to_string(MODULE_CONNECT_TIMEOUT.count()) + " seconds");
            }

            server::check();
            tub::check();
            client::check();

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            attempts++;
        }
    }

    // wait until init.finish is sent
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
}

static void init_layout()
{
    std::stringstream ss;
    bool first = true;

    std::size_t size = 0;
    for (std::size_t id = 1; id < MAX_MODULES; id++)
    {
        const auto settings = settings::get_module_settings(module::ModuleId(id));
        if (!settings.active)
            continue;

        if (!first)
            ss << "|";

        first = false;

        ss << id << ":"
           << (settings.left ? settings.left->raw() : 0) << ","
           << (settings.middle ? settings.middle->raw() : 0) << ","
           << (settings.right ? settings.right->raw() : 0);
        size++;
    }
    std::string data = ss.str();

    for (const auto &module : module::get_modules())
    {
        if (!module)
        {
            continue;
        }

        module->send(message::UpdateGraph(module->id(), size, data));
    }
}

static void init_schedule(const std::list<std::unique_ptr<events::Event>> &events)
{
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
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char const *argv[])
{
    for (int i = 1; i < argc; i++)
    {
        const std::string_view arg(argv[i]);
        if (arg == "-v" || arg == "--version")
        {
            std::cout << "Quercus Airport Simulation version " << APP_VERSION
                      << " (" << get_target_os() << " " << get_target_arch() << ")" << std::endl;
            return EXIT_SUCCESS;
        }
        else if (arg == "-g" || arg == "--no-gui")
        {
            using_visualization = false;
            continue;
        }
        else if (arg != "-h" || arg != "--help")
        {
            std::cout << "Invalid option: " << arg << std::endl;
        }

        std::cout << "Use: ./simulation [options]" << std::endl;
        std::cout << "Options:" << std::endl;
        std::cout << "  -h, --help              Prints all options" << std::endl;
        std::cout << "  -g, --no-gui            Disables GUI" << std::endl;
        std::cout << "  -v, --version           Prints version" << std::endl;
        std::cout << std::endl;
        std::cout << "Report bugs and provide suggestions at <https://gitlab.tue.nl/2irr70-capstone-quercus-airport/simulation/-/work_items>" << std::endl;
        return EXIT_SUCCESS;
    }

    ::signal(SIGTERM, handle_close);

    settings::parse_settings("settings.toml", "layout.toml");
    std::list<std::unique_ptr<events::Event>> events;
    events::parse_events("events.toml", events);
    server::run();

    init_modules();

    init_layout();
    init_schedule(events);

    virtual_clock::VirtualClock vclock = virtual_clock::VirtualClock();
    vclock.set_simulation_speed(settings::get_setting<double>("simulation_speed"));

    if (using_visualization)
        visualization::init();
    while (is_running)
    {
        try
        {
            events::fire_events(vclock, events);
            if (using_visualization)
                visualization::update(vclock);
            vclock.tick();

            server::check();
            tub::check();
            client::check();

            is_running = visualization::is_running();

            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(FPS_TARGET / 1000.0)));
        }
        catch (std::exception &ex)
        {
            std::cerr << "Shutting down due to failed check: " << ex.what() << std::endl;

            tub::stop();
            tub::join();
            server::stop();
            server::join();
            client::stop();
            if (using_visualization)
                visualization::stop();
            return EXIT_FAILURE;
        }
    }

    tub::stop();
    tub::join();
    server::stop();
    server::join();
    client::stop();
    if (using_visualization)
        visualization::stop();

    return EXIT_SUCCESS;
}
