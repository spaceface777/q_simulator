#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "../defs.h"
#include "../event/events.h"
#include "../ltl/parser.h"
#include "../settings.h"
#include "../sim/runtime.h"
#include "../util.h"

namespace {

	// Simulated time granted per iteration.
	constexpr std::chrono::microseconds SLICE{50000};

	// How long a slice waits on child applications before the runner gives up on keeping pace.
	constexpr std::chrono::milliseconds CHILD_BUDGET{5000};

	constexpr int EXIT_PROPERTY_FAILED = 1;
	constexpr int EXIT_CONFIGURATION   = 2;

	struct Options {
		std::string             scenario = "events.toml";
		std::string             settings = "settings.toml";
		std::string             layout   = "layout.toml";
		std::string             binary;
		std::string             trace;
		std::optional<uint64_t> seed;
		std::optional<double>   timeout;
		uint32_t                tick_batch   = 1;
		bool                    quiet        = false;
		bool                    layout_given = false;
	};

	void usage() {
		std::cout << "Use: ./simulation-scenario [options] [scenario.toml]\n"
				  << "\n"
				  << "Runs a scenario against an application with no GUI and no operator, and\n"
				  << "exits non-zero if any temporal property does not hold.\n"
				  << "\n"
				  << "Options:\n"
				  << "  -b, --binary <path>     The application to run (default: scenario or settings)\n"
				  << "  -s, --settings <path>   The settings file (default: settings.toml)\n"
				  << "  -l, --layout <path>     The layout file (default: scenario or layout.toml)\n"
				  << "      --seed <n>          The master seed, overriding the one in settings\n"
				  << "      --timeout <sec>     Override the scenario's simulated-time limit\n"
				  << "      --tick-batch <n>    Run up to n child ticks per barrier (default: 1)\n"
				  << "      --trace <path>      Write an NDJSON trace of the run\n"
				  << "  -q, --quiet             Print only failures\n"
				  << "  -h, --help              Print all options\n"
				  << "  -v, --version           Print version\n"
				  << "\n"
				  << "A scenario may set [scenario].layout and [scenario].binary. Relative paths\n"
				  << "are resolved from the scenario's folder; command-line options override them.\n"
				  << "\n"
				  << "Exit codes:\n"
				  << "  0  every property held\n"
				  << "  1  a property did not hold, or the run could not be completed\n"
				  << "  2  the scenario or configuration could not be read\n"
				  << std::endl;
	}

	// Reads the next argument of an option that takes one. Throws std::invalid_argument if missing.
	std::string take_value(int argc, char const* argv[], int& index, const std::string_view option) {
		util::require(index + 1 < argc, "Missing value for option " + std::string(option));
		index++;
		return argv[index];
	}

	// Returns nothing when the command line asked for help or a version. Throws
	// std::invalid_argument if an option is unknown or malformed.
	std::optional<Options> parse_arguments(int argc, char const* argv[]) {
		Options options;
		bool    scenario_given = false;

		for (int index = 1; index < argc; index++) {
			const std::string_view argument(argv[index]);

			if (argument == "-h" || argument == "--help") {
				usage();
				return std::nullopt;
			}
			if (argument == "-v" || argument == "--version") {
				std::cout << "Quercus Airport Simulation scenario runner version " << APP_VERSION << std::endl;
				return std::nullopt;
			}
			if (argument == "-b" || argument == "--binary") {
				options.binary = take_value(argc, argv, index, argument);
			} else if (argument == "-s" || argument == "--settings") {
				options.settings = take_value(argc, argv, index, argument);
			} else if (argument == "-l" || argument == "--layout") {
				options.layout       = take_value(argc, argv, index, argument);
				options.layout_given = true;
			} else if (argument == "--trace") {
				options.trace = take_value(argc, argv, index, argument);
			} else if (argument == "--seed") {
				const std::string        value = take_value(argc, argv, index, argument);
				const std::optional<int> seed  = util::parse_int(value);
				util::require(seed.has_value() && *seed >= 0, "Invalid seed: " + value);
				options.seed = static_cast<uint64_t>(*seed);
			} else if (argument == "--timeout") {
				const std::string           value   = take_value(argc, argv, index, argument);
				const std::optional<double> timeout = util::parse_double(value);
				util::require(timeout.has_value() && std::isfinite(*timeout) && *timeout > 0.0,
							  "Invalid timeout: " + value);
				options.timeout = *timeout;
			} else if (argument == "--tick-batch") {
				const std::string        value = take_value(argc, argv, index, argument);
				const std::optional<int> batch = util::parse_int(value);
				util::require(batch.has_value() && *batch > 0 && static_cast<uint32_t>(*batch) <= sim::MAX_TICK_BATCH,
							  "Tick batch must be between 1 and " + std::to_string(sim::MAX_TICK_BATCH));
				options.tick_batch = static_cast<uint32_t>(*batch);
			} else if (argument == "-q" || argument == "--quiet") {
				options.quiet = true;
			} else if (!argument.empty() && argument.front() == '-') {
				throw std::invalid_argument("Unknown option: " + std::string(argument));
			} else {
				util::require(!scenario_given, "More than one scenario given");
				options.scenario = argument;
				scenario_given   = true;
			}
		}

		return options;
	}

	std::string read_file(const std::filesystem::path& path) {
		std::ifstream input(path);
		util::require(input.is_open(), "Could not open " + path.string());
		std::ostringstream contents;
		contents << input.rdbuf();
		return contents.str();
	}

	std::string timestamp(uint64_t tick) {
		std::ostringstream output;
		output << std::fixed << std::setprecision(3)
			   << (static_cast<double>(tick) * static_cast<double>(sim::TICK_DURATION.count())) / 1'000'000.0 << " s";
		return output.str();
	}

} // namespace

int main(int argc, char const* argv[]) {
	std::optional<Options> parsed;
	try {
		parsed = parse_arguments(argc, argv);
	} catch (const std::exception& error) {
		std::cerr << error.what() << std::endl;
		usage();
		return EXIT_CONFIGURATION;
	}

	if (!parsed) {
		return EXIT_SUCCESS;
	}
	const Options& options = *parsed;
	sim::pin_to_one_cpu();

	std::filesystem::path                layout_file = options.layout;
	std::optional<std::filesystem::path> binary_file;
	events::Schedule                     schedule;
	try {
		const events::ScenarioFiles files = events::parse_scenario_files(options.scenario);
		if (!options.layout_given && files.layout) layout_file = *files.layout;
		binary_file = options.binary.empty() ? files.binary : std::optional<std::filesystem::path>(options.binary);

		settings::parse_settings(options.settings, layout_file, binary_file);
		if (options.seed) {
			settings::set_setting<int>("seed", static_cast<int>(*options.seed));
		}
		schedule = events::parse_events(options.scenario, static_cast<uint64_t>(settings::get_setting<int>("seed")));
	} catch (const std::exception& error) {
		std::cerr << "Could not read the scenario: " << error.what() << std::endl;
		return EXIT_CONFIGURATION;
	}

	if (!schedule.properties) {
		std::cerr << options.scenario << " is missing [scenario].duration and [scenario].properties" << std::endl;
		return EXIT_CONFIGURATION;
	}

	ltl::File properties;
	try {
		properties =
			ltl::parse(read_file(schedule.properties->property_file), schedule.properties->property_file.string());
	} catch (const std::exception& error) {
		std::cerr << "Could not read the properties: " << error.what() << std::endl;
		return EXIT_CONFIGURATION;
	}

	const uint64_t master_seed = static_cast<uint64_t>(settings::get_setting<int>("seed"));

	world::configure_calibration_from_settings();

	// Each module draws its own calibration from the physical population; no two belts are the
	// same on the real platform either. The master seed keeps the fleet reproducible.
	world::set_calibration_seed(master_seed);

	sim::Runtime runtime(master_seed, sim::CONNECT_TIMEOUT, sim::ACTIVATION_TIMEOUT, options.tick_batch);
	uint64_t     timeout_tick = schedule.properties->duration_tick;
	if (options.timeout) {
		const double ticks = (*options.timeout * 1'000'000.0) / static_cast<double>(sim::TICK_DURATION.count());
		util::require(ticks <= static_cast<double>(std::numeric_limits<uint64_t>::max()), "Timeout is too large");
		timeout_tick = static_cast<uint64_t>(ticks);
	}

	try {
		runtime.load(schedule);
		runtime.load_properties(std::move(properties), schedule.properties->duration_tick,
								schedule.properties->property_file.string());
	} catch (const std::exception& error) {
		std::cerr << "Could not configure the properties: " << error.what() << std::endl;
		return EXIT_CONFIGURATION;
	}

	try {
		runtime.trace_to(options.trace, {options.settings, layout_file.string(), options.scenario,
										 schedule.properties->property_file.string()});
		runtime.start(timeout_tick);
	} catch (const std::exception& error) {
		std::cerr << "Failed to start the simulation: " << error.what() << std::endl;
		runtime.stop();
		return EXIT_PROPERTY_FAILED;
	}

	if (!options.quiet) {
		std::cout << "Running " << options.scenario << " with " << runtime.properties_total() << " propert"
				  << (runtime.properties_total() == 1 ? "y" : "ies") << std::endl;
	}

	bool timed_out = false;

	try {
		while (!runtime.failed() && runtime.properties_pending() > 0) {
			if (runtime.tick() > timeout_tick) {
				timed_out = true;
				break;
			}

			const uint64_t before        = runtime.tick();
			const uint64_t ticks_left    = timeout_tick - before + 1;
			const uint64_t slice_ticks   = static_cast<uint64_t>(SLICE.count() / sim::TICK_DURATION.count());
			uint64_t       granted_ticks = std::min(ticks_left, slice_ticks);
			if (granted_ticks % options.tick_batch != 0) {
				granted_ticks += options.tick_batch - (granted_ticks % options.tick_batch);
			}
			runtime.advance(std::chrono::microseconds(granted_ticks * sim::TICK_DURATION.count()), 1.0, CHILD_BUDGET);

			// No progress means a wedged child; stop now or the run would spin without a timeout.
			if (runtime.tick() == before) {
				std::cerr << "The applications stopped making progress at tick " << runtime.tick() << std::endl;
				timed_out = true;
				break;
			}
		}
	} catch (const std::exception& error) {
		std::cerr << "The run ended at tick " << runtime.tick() << ": " << error.what() << std::endl;
		runtime.stop();
		return EXIT_PROPERTY_FAILED;
	}

	const std::size_t                      unclear = runtime.properties_unresolved();
	const bool                             failed  = runtime.failed() || timed_out || unclear > 0;
	const std::size_t                      total   = runtime.properties_total();
	const std::size_t                      broken  = runtime.properties_failed();
	const uint64_t                         ticks   = runtime.tick();
	const std::vector<sim::PropertyResult> results = runtime.property_results();

	runtime.stop();

	for (const sim::PropertyResult& result : results) {
		if (result.verdict == ltl::Verdict::TRUE) {
			// A conditional property reports how often it fired. Zero is a pass that never tested
			// anything, and reads differently from a pass that tested a hundred times.
			if (!options.quiet) {
				std::cout << "PASS " << result.name;
				if (result.has_trigger) std::cout << " (triggered " << result.activations << " times)";
				std::cout << std::endl;
			}
		} else if (result.verdict == ltl::Verdict::UNRESOLVED) {
			std::cerr << "UNRESOLVED " << result.name << " at scenario end " << timestamp(ticks);
			if (result.has_trigger) std::cerr << " (triggered " << result.activations << " times)";
			std::cerr << std::endl;
			std::cerr << "  source: " << result.source << ":" << result.position.line << ":" << result.position.column
					  << std::endl;
			std::cerr << "  the trace ended before this could be decided; it was not refuted" << std::endl;
			if (!result.obligation_at_end.empty()) {
				std::cerr << "  still waiting on: " << result.obligation_at_end << std::endl;
			}
		} else if (result.verdict == ltl::Verdict::FALSE) {
			std::cerr << "FAIL " << result.name;
			if (result.decisive_tick) std::cerr << " at " << timestamp(*result.decisive_tick);
			std::cerr << std::endl;
			if (result.failure) {
				std::cerr << "  source: " << result.source << ":" << result.position.line << ":"
						  << result.position.column << std::endl;
				if (result.failure->obligation_created_tick) {
					std::cerr << "  obligation created at " << timestamp(*result.failure->obligation_created_tick)
							  << std::endl;
				}
				if (result.failure->deadline_tick) {
					std::cerr << "  deadline: " << timestamp(*result.failure->deadline_tick) << std::endl;
				}
				std::cerr << "  formula: " << result.failure->formula << std::endl;
				std::cerr << "  obligation: " << result.failure->obligation << std::endl;
				for (const ltl::AtomFact& fact : result.failure->facts) {
					std::cerr << "  " << fact.expression << " = " << (fact.value ? "true" : "false");
					if (fact.actual) std::cerr << ", was " << *fact.actual;
					std::cerr << std::endl;
				}
			}
		} else if (timed_out) {
			if (options.timeout) {
				std::cerr << "CANCELLED " << result.name << " at " << timestamp(timeout_tick) << " (still pending)"
						  << std::endl;
			} else {
				std::cerr << "UNRESOLVED " << result.name << " at scenario end " << timestamp(timeout_tick)
						  << std::endl;
			}
		}
	}

	if (failed) {
		if (timed_out) {
			if (options.timeout) {
				std::cerr << "The run timed out with " << runtime.properties_pending() << " of " << total
						  << " properties still pending after " << ticks << " ticks" << std::endl;
			} else {
				std::cerr << "The scenario ended with " << runtime.properties_pending() << " of " << total
						  << " properties unresolved after " << ticks << " ticks" << std::endl;
			}
		} else if (broken == 0 && unclear > 0) {
			std::cerr << unclear << " of " << total << " properties were undecided when the trace ended after " << ticks
					  << " ticks; extend the scenario or bound the obligation" << std::endl;
		} else {
			std::cerr << broken << " of " << total << " properties failed after " << ticks << " ticks";
			if (unclear > 0) std::cerr << ", and " << unclear << " were left undecided";
			std::cerr << std::endl;
		}
		return EXIT_PROPERTY_FAILED;
	}

	if (!options.quiet) {
		std::cout << "All " << total << " properties held after " << ticks << " ticks" << std::endl;
	}
	return EXIT_SUCCESS;
}
