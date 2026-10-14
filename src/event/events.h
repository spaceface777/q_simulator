#ifndef EVENTS_H
#define EVENTS_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "../defs.h"
#include "../module/types.h"
#include "../world/world.h"

namespace events {

	namespace fs = std::filesystem;

	struct TubArrival {
		uint64_t         tick = 0;
		uint8_t          id   = 0;
		module::ModuleId module;

		// Lets a scenario place a tub directly at a beam or belt, not only at the entry reader.
		world::Position position = world::Position::ENTRY;

		// Removal is its own stimulus: an application can react to a beam clearing.
		std::optional<uint64_t> removed_tick;

		RfidUid     uid{};
		RfidPayload payload{};
	};

	struct PropertyConfiguration {
		fs::path property_file;
		uint64_t duration_tick = 0;
	};

	// Files a scenario can select for a self-contained run. Relative paths are resolved from the
	// scenario file, just like the sibling LTLf property file.
	struct ScenarioFiles {
		std::optional<fs::path>            layout;
		std::optional<fs::path>            firmware;
		std::map<std::string, std::string> environment;
	};

	struct Schedule {
		std::vector<TubArrival>              tubs;
		std::optional<PropertyConfiguration> properties;

		// Zero for an empty schedule.
		[[nodiscard]] uint64_t last_tick() const noexcept;
	};

	[[nodiscard]] ScenarioFiles parse_scenario_files(const fs::path& file);
	[[nodiscard]] Schedule      parse_events(const fs::path& file, uint64_t master_seed = 0);

} // namespace events

#endif
