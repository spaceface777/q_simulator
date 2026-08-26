#include <chrono>
#include <csignal>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

#include "defs.h"
#include "event/events.h"
#include "settings.h"
#include "sim/runtime.h"
#include "synchronization/virtual_clock.h"
#include "util.h"
#include "visualization/visualization.h"

static constexpr double FPS_TARGET = 60;

// Frame pacing is deadline based: simulation and rendering time are part of the frame, rather
// than extra time added before a fixed sleep. Nanoseconds retain the fractional part of 60 Hz.
static constexpr auto FRAME_DURATION = std::chrono::nanoseconds(static_cast<int64_t>(1000000000.0 / FPS_TARGET));

// Keep a small, fixed slice for rendering and input; simulation may use the rest of the frame.
static constexpr auto RENDER_RESERVE = std::chrono::milliseconds{2};
static constexpr auto CHILD_BUDGET =
	std::chrono::duration_cast<std::chrono::microseconds>(FRAME_DURATION - RENDER_RESERVE);

constexpr std::string_view get_target_os() {
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

constexpr std::string_view get_target_arch() {
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
static bool is_running          = true;

static void handle_close(int code) {
	if (using_visualization) visualization::stop();

	is_running = false;

	::_exit(code);
}

int main([[maybe_unused]] int argc, [[maybe_unused]] char const* argv[]) {
	std::chrono::milliseconds timeout    = std::chrono::milliseconds(std::numeric_limits<int64_t>::max());
	uint32_t                  tick_batch = 1;

	for (int i = 1; i < argc; i++) {
		const std::string_view arg(argv[i]);
		if (arg == "-v" || arg == "--version") {
			std::cout << "Quercus Airport Simulation version " << APP_VERSION << " (" << get_target_os() << " "
					  << get_target_arch() << ")" << std::endl;
			return EXIT_SUCCESS;
		} else if (arg == "--no-gui") {
			using_visualization = false;
			continue;
		} else if (arg == "--timeout") {
			if (i + 1 < argc) {
				const auto timeout_opt = util::parse_double(argv[i + 1]);
				if (timeout_opt) {
					timeout = std::chrono::milliseconds(static_cast<int64_t>(*timeout_opt * 1000));
					i++;
					continue;
				}
				std::cout << "Invalid timeout: " << argv[i + 1] << std::endl;
				return EXIT_SUCCESS;
			} else {
				std::cout << "Missing <sec> for option '--timeout'" << std::endl;
				return EXIT_SUCCESS;
			}
		} else if (arg == "--tick-batch") {
			if (i + 1 < argc) {
				const auto batch = util::parse_int(argv[i + 1]);
				if (batch && *batch > 0 && static_cast<uint32_t>(*batch) <= sim::MAX_TICK_BATCH) {
					tick_batch = static_cast<uint32_t>(*batch);
					i++;
					continue;
				}
				std::cout << "Tick batch must be between 1 and " << sim::MAX_TICK_BATCH << std::endl;
				return EXIT_FAILURE;
			}
			std::cout << "Missing <n> for option '--tick-batch'" << std::endl;
			return EXIT_FAILURE;
		}

		// Asking for help is not an error and an unknown option is, so the two cannot share an
		// exit code. They previously shared a branch as well: `arg != "-h" || arg != "--help"`
		// is true for every string, so `-h` reported itself as invalid.
		const bool help = arg == "-h" || arg == "--help";
		if (!help) {
			std::cout << "Invalid option: " << arg << std::endl;
		}

		std::cout << "Use: ./simulation [options]" << std::endl;
		std::cout << "Options:" << std::endl;
		std::cout << "  -h, --help              Prints all options" << std::endl;
		std::cout << "  --no-gui                Runs without a window" << std::endl;
		std::cout << "  --timeout <sec>         Closes the simulation after <sec> seconds" << std::endl;
		std::cout << "  --tick-batch <n>        Runs up to n child ticks per barrier" << std::endl;
		std::cout << "  -v, --version           Prints version" << std::endl;
		std::cout << std::endl;
		std::cout << "To run a scenario to a pass or fail verdict with no window and no" << std::endl;
		std::cout << "operator, use ./simulation-scenario instead." << std::endl;
		std::cout << std::endl;
		std::cout << "Report bugs and provide suggestions at "
					 "<https://gitlab.tue.nl/2irr70-capstone-quercus-airport/simulation/-/work_items>"
				  << std::endl;
		return help ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	::signal(SIGTERM, handle_close);

	settings::parse_settings("settings.toml", "layout.toml");
	const uint64_t         master_seed = static_cast<uint64_t>(settings::get_setting<int>("seed"));
	const events::Schedule schedule    = events::parse_events("events.toml", master_seed);

	// An interactive run is a run of the platform, not of one reference module, so every module
	// gets its own calibration drawn from the physical population. The master seed makes that
	// fleet reproducible: the same seed is the same seventeen modules.
	world::configure_calibration_from_settings();
	world::set_calibration_seed(master_seed);

	sim::Runtime runtime(master_seed, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, tick_batch);

	try {
		runtime.load(schedule);
		runtime.trace_to(settings::get_setting<std::string>("trace_path"),
						 {"settings.toml", "layout.toml", "events.toml"});
		runtime.start();
	} catch (const std::exception& ex) {
		std::cerr << "Failed to start the simulation: " << ex.what() << std::endl;
		return EXIT_FAILURE;
	}

	virtual_clock::VirtualClock vclock = virtual_clock::VirtualClock();
	vclock.set_simulation_speed(settings::get_setting<double>("simulation_speed"));

	if (using_visualization) visualization::init();

	auto previous = std::chrono::steady_clock::now();
	while (is_running) {
		if (vclock.get_virtual_time() > timeout) break;

		try {
			const auto now      = std::chrono::steady_clock::now();
			const auto deadline = now + FRAME_DURATION;
			const auto elapsed  = std::chrono::duration_cast<std::chrono::microseconds>(now - previous);
			previous            = now;

			runtime.advance(elapsed, vclock.simulation_speed(), CHILD_BUDGET);

			// The clock reports the engine's tick rather than scaled real time, so what the
			// visualization shows is the time the applications actually experienced.
			vclock.set_virtual_time(std::chrono::milliseconds(static_cast<int64_t>(runtime.tick())));

			// Without a window there is nothing that can ask the run to stop, so the loop ends
			// at the timeout rather than on a closed window.
			if (using_visualization) {
				visualization::update(runtime.world(), runtime.platform(), vclock);
				is_running = visualization::is_running();
			}

			std::this_thread::sleep_until(deadline);
		} catch (std::exception& ex) {
			std::cerr << "Shutting down due to failed check: " << ex.what() << std::endl;

			runtime.stop();
			if (using_visualization) visualization::stop();
			return EXIT_FAILURE;
		}
	}

	runtime.stop();
	if (using_visualization) visualization::stop();

	return EXIT_SUCCESS;
}
