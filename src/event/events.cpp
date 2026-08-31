#include "events.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <toml++/toml.hpp>
#include <unordered_set>

#include "../ltl/parser.h"
#include "../seed/seed.h"
#include "../util.h"

namespace events {

	namespace {

		// Scenarios are written in seconds; the engine works in ticks.
		constexpr double TICKS_PER_SECOND = 1000.0;

		uint8_t parse_byte(const toml::node* node, const std::string& context) {
			const std::optional<int64_t> value = node == nullptr ? std::nullopt : node->value<int64_t>();
			if (!value || *value < 0 || *value > 0xFF) {
				throw std::invalid_argument(context + " must be a byte between 0x00 and 0xff");
			}
			return static_cast<uint8_t>(*value);
		}

		template <std::size_t Size>
		std::array<uint8_t, Size> parse_byte_array(const toml::array* array, const std::string& context) {
			if (array == nullptr || array->size() != Size) {
				throw std::invalid_argument(context + " must contain exactly " + std::to_string(Size) + " bytes");
			}
			std::array<uint8_t, Size> result{};
			for (std::size_t index = 0; index < Size; index++) {
				result[index] = parse_byte(array->get(index), context);
			}
			return result;
		}

		RfidUid parse_uid(const toml::node* node) {
			if (node == nullptr) throw std::invalid_argument("Tub event missing its uid");
			if (const toml::array* bytes = node->as_array()) {
				return parse_byte_array<RFID_UID_SIZE_BYTES>(bytes, "uid");
			}
			const std::optional<int64_t> value = node->value<int64_t>();
			if (!value || *value < 0 || static_cast<uint64_t>(*value) > 0x00FFFFFFFFFFFFFFULL) {
				throw std::invalid_argument("uid must be a non-negative 7-byte integer or a 7-byte array");
			}
			RfidUid  result{};
			uint64_t remaining = static_cast<uint64_t>(*value);
			for (std::size_t index = RFID_UID_SIZE_BYTES; index-- > 0;) {
				result[index] = static_cast<uint8_t>(remaining & 0xFFU);
				remaining >>= 8U;
			}
			return result;
		}

		RfidPage parse_page(const toml::node& node, std::size_t index) {
			const std::string context = "payload page " + std::to_string(index);
			if (const toml::array* bytes = node.as_array()) {
				return parse_byte_array<RFID_PAGE_SIZE_BYTES>(bytes, context);
			}
			const std::optional<int64_t> value = node.value<int64_t>();
			if (!value || *value < 0 || static_cast<uint64_t>(*value) > 0xFFFFFFFFULL) {
				throw std::invalid_argument(context + " must be a non-negative 4-byte integer or a 4-byte array");
			}
			RfidPage result{};
			uint64_t remaining = static_cast<uint64_t>(*value);
			for (std::size_t byte = RFID_PAGE_SIZE_BYTES; byte-- > 0;) {
				result[byte] = static_cast<uint8_t>(remaining & 0xFFU);
				remaining >>= 8U;
			}
			return result;
		}

		RfidPayload parse_payload(const toml::array* pages, uint64_t master_seed, uint8_t tub_id) {
			if (pages == nullptr) throw std::invalid_argument("Tub event missing its payload array");
			if (pages->size() > RFID_PAYLOAD_PAGE_COUNT) {
				throw std::invalid_argument("payload contains more than " + std::to_string(RFID_PAYLOAD_PAGE_COUNT) +
											" pages");
			}
			RfidPayload result{};
			for (std::size_t index = 0; index < pages->size(); index++) {
				result[index] = parse_page(*pages->get(index), index);
			}
			for (std::size_t index = pages->size(); index < RFID_PAYLOAD_PAGE_COUNT; index++) {
				seed::SplitMix64 stream(seed::derive(master_seed, "tub/" + std::to_string(tub_id) +
																	  "/rfid-payload/page/" + std::to_string(index)));
				const uint64_t   random = stream.next();
				for (std::size_t byte = 0; byte < RFID_PAGE_SIZE_BYTES; byte++) {
					result[index][byte] = static_cast<uint8_t>(random >> (8U * (RFID_PAGE_SIZE_BYTES - 1 - byte)));
				}
			}
			return result;
		}

	} // namespace

	ScenarioFiles parse_scenario_files(const fs::path& file) {
		util::require(fs::exists(file), "No scenario file found at " + file.string());

		const toml::table table = toml::parse_file(file.string());
		ScenarioFiles     files;
		const toml::node* node = table.get("scenario");
		if (node == nullptr) return files;

		const toml::table* scenario = node->as_table();
		util::require(scenario != nullptr, "scenario must be a TOML table");

		auto read_path = [&](const std::string_view key) -> std::optional<fs::path> {
			const toml::node* value_node = scenario->get(key);
			if (value_node == nullptr) return std::nullopt;

			const std::optional<std::string> value = value_node->value<std::string>();
			util::require(value.has_value() && !value->empty(), "[scenario]." + std::string(key) + " must name a file");
			fs::path path(*value);
			if (path.is_relative()) path = file.parent_path() / path;
			return path.lexically_normal();
		};

		files.layout = read_path("layout");
		files.binary = read_path("binary");
		return files;
	}

	uint64_t Schedule::last_tick() const noexcept {
		uint64_t last = 0;
		for (const TubArrival& tub : tubs) {
			last = std::max(last, tub.removed_tick.value_or(tub.tick));
		}
		return last;
	}

	Schedule parse_events(const fs::path& file, uint64_t master_seed) {
		util::require(fs::exists(file), "No events.toml found in the same folder as the simulation executable");

		const toml::table table = toml::parse_file(file.string());
		Schedule          schedule;

		util::require(table.get("assert") == nullptr,
					  "[assert.*] is no longer supported; declare [scenario].properties with an LTLf .spec file");

		if (const toml::node* node = table.get("scenario"); node != nullptr) {
			const toml::table* scenario = node->as_table();
			util::require(scenario != nullptr, "scenario must be a TOML table");

			const std::optional<std::string> duration   = (*scenario)["duration"].value<std::string>();
			const std::optional<std::string> properties = (*scenario)["properties"].value<std::string>();
			util::require(duration.has_value(), "[scenario].duration must be a quoted, unit-bearing duration");
			util::require(properties.has_value() && !properties->empty(),
						  "[scenario].properties must name an LTLf .spec file");

			PropertyConfiguration configuration;
			configuration.duration_tick = ltl::parse_duration(*duration, file.string() + ":[scenario].duration");
			configuration.property_file = fs::path(*properties);
			if (configuration.property_file.is_relative()) {
				configuration.property_file = file.parent_path() / configuration.property_file;
			}
			configuration.property_file = configuration.property_file.lexically_normal();
			schedule.properties         = std::move(configuration);
		}

		const toml::node* group = table.get("tub");
		if (group == nullptr || !group->is_table()) {
			return schedule;
		}

		std::unordered_set<uint8_t> tub_ids;
		for (auto&& [key, value] : *group->as_table()) {
			(void)key;
			if (!value.is_table()) {
				continue;
			}
			const toml::table&                           entry          = *value.as_table();
			static const std::unordered_set<std::string> allowed_fields = {"time",    "id",  "module", "position",
																		   "removed", "uid", "payload"};
			for (auto&& [field, ignored] : entry) {
				(void)ignored;
				util::require(allowed_fields.count(std::string(field.str())) != 0,
							  "Unknown tub field: " + std::string(field.str()));
			}

			const double  time      = entry["time"].value_or(-1.0);
			const int64_t id        = entry["id"].value_or(static_cast<int64_t>(-1));
			const int64_t module_id = entry["module"].value_or(static_cast<int64_t>(-1));
			util::require(id >= 1 && id <= 255, "Tub ids must be between 1 and 255");
			util::require(module_id >= 1 && module_id < MAX_MODULES,
						  "Tub modules must be between 1 and " + std::to_string(MAX_MODULES - 1));
			util::require(tub_ids.insert(static_cast<uint8_t>(id)).second,
						  "Tub id " + std::to_string(id) + " is reused; tub ids are single-use within a scenario");
			util::require(time >= 0.0, "Tub event missing its time");

			TubArrival tub;
			tub.tick    = static_cast<uint64_t>(std::llround(time * TICKS_PER_SECOND));
			tub.id      = static_cast<uint8_t>(id);
			tub.module  = module::ModuleId{static_cast<uint8_t>(module_id)};
			tub.uid     = parse_uid(entry.get("uid"));
			tub.payload = parse_payload(entry["payload"].as_array(), master_seed, tub.id);

			if (const std::optional<std::string> position = entry["position"].value<std::string>()) {
				const std::optional<world::Position> parsed = world::position_from_string(*position);
				util::require(parsed.has_value(),
							  "Tub " + std::to_string(id) + " has an unknown position: " + *position);
				tub.position = *parsed;
			}

			if (const std::optional<double> removed = entry["removed"].value<double>()) {
				util::require(*removed > time, "Tub " + std::to_string(id) + " is removed before it arrives");
				tub.removed_tick = static_cast<uint64_t>(std::llround(*removed * TICKS_PER_SECOND));
			}

			schedule.tubs.push_back(tub);
		}

		// Ordered by tick, then by id, so that the schedule does not depend on the order the file
		// happened to list its entries in.
		std::sort(schedule.tubs.begin(), schedule.tubs.end(), [](const TubArrival& left, const TubArrival& right) {
			return left.tick != right.tick ? left.tick < right.tick : left.id < right.id;
		});

		return schedule;
	}

} // namespace events
