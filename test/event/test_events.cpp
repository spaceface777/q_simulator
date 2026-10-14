#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include "../../src/event/events.h"

namespace fs = std::filesystem;

namespace {

	const std::string ZERO_PAYLOAD = R"(payload = [
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
])";

	std::string tub(double time = 1.5, int id = 3, std::string extra = {}) {
		return "time = " + std::to_string(time) + "\nid = " + std::to_string(id) +
			   "\nmodule = 1\nuid = 0x11223344556677\n" + ZERO_PAYLOAD + "\n" + extra;
	}

	class EventsTest : public ::testing::Test {
	protected:
		fs::path path;

		void SetUp() override {
			std::random_device                 device;
			std::mt19937                       generator(device());
			std::uniform_int_distribution<int> distribution(1, 1000000);
			path = fs::temp_directory_path() / ("events_" + std::to_string(distribution(generator)) + ".toml");
		}

		void TearDown() override { fs::remove(path); }

		void write(const std::string& contents) {
			std::ofstream file(path);
			file << contents;
		}
	};

} // namespace

TEST_F(EventsTest, ParsesIntegerUidAndMixedPageRepresentations) {
	std::string contents = R"(
[tub.first]
time = 1.5
id = 3
module = 12
uid = 0x11223344556677
payload = [
    0x11223344, [0x55, 0x66, 0x77, 0x88],
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
]
)";
	write(contents);

	const events::Schedule schedule = events::parse_events(path);
	ASSERT_EQ(schedule.tubs.size(), 1u);
	const events::TubArrival& parsed = schedule.tubs[0];
	EXPECT_EQ(parsed.tick, 1500u);
	EXPECT_EQ(parsed.id, 3);
	EXPECT_EQ(parsed.module.raw(), 12);
	EXPECT_EQ(parsed.uid, (RfidUid{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77}));
	EXPECT_EQ(parsed.payload[0], (RfidPage{0x11, 0x22, 0x33, 0x44}));
	EXPECT_EQ(parsed.payload[1], (RfidPage{0x55, 0x66, 0x77, 0x88}));
	EXPECT_EQ(schedule.last_tick(), 1500u);
}

TEST_F(EventsTest, ParsesArrayUid) {
	std::string contents = tub();
	contents.replace(contents.find("uid = 0x11223344556677"), 22, "uid = [1, 2, 3, 4, 5, 6, 7]");
	write("[tub.first]\n" + contents);
	const events::Schedule schedule = events::parse_events(path);
	ASSERT_EQ(schedule.tubs.size(), 1u);
	EXPECT_EQ(schedule.tubs[0].uid, (RfidUid{1, 2, 3, 4, 5, 6, 7}));
}

TEST_F(EventsTest, OrdersBySchedulingRatherThanFileOrder) {
	write("[tub.later]\n" + tub(2.0, 9) + "\n[tub.earlier]\n" + tub(0.1, 4));
	const events::Schedule schedule = events::parse_events(path);
	ASSERT_EQ(schedule.tubs.size(), 2u);
	EXPECT_EQ(schedule.tubs[0].id, 4);
	EXPECT_EQ(schedule.tubs[1].id, 9);
}

TEST_F(EventsTest, RejectsReusingATubIdAfterRemoval) {
	write("[tub.first]\n" + tub(0.0, 7, "removed = 1.0\n") + "\n[tub.second]\n" + tub(2.0, 7));
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
}

TEST_F(EventsTest, PlacesAtReaderUnlessToldOtherwise) {
	write("[tub.default]\n" + tub(0.0, 1) + "\n[tub.beam]\n" + tub(1.0, 2, "position = \"left_laser\"\n"));
	const events::Schedule schedule = events::parse_events(path);
	ASSERT_EQ(schedule.tubs.size(), 2u);
	EXPECT_EQ(schedule.tubs[0].position, world::Position::ENTRY);
	EXPECT_EQ(schedule.tubs[1].position, world::Position::LEFT_LASER);
}

TEST_F(EventsTest, ReadsRemovalAndExtendsLastTick) {
	write("[tub.first]\n" + tub(1.0, 3, "removed = 4.5\n"));
	const events::Schedule schedule = events::parse_events(path);
	ASSERT_TRUE(schedule.tubs[0].removed_tick.has_value());
	EXPECT_EQ(*schedule.tubs[0].removed_tick, 4500u);
	EXPECT_EQ(schedule.last_tick(), 4500u);
}

TEST_F(EventsTest, RejectsRemovedBeforeArrival) {
	write("[tub.first]\n" + tub(5.0, 3, "removed = 2.0\n"));
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
}

TEST_F(EventsTest, RejectsMissingOrWrongLengthUid) {
	write("[tub.first]\ntime = 0\nid = 1\nmodule = 1\n" + ZERO_PAYLOAD);
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
	write("[tub.first]\n" + tub().replace(tub().find("0x11223344556677"), 16, "[1, 2, 3]"));
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
}

TEST_F(EventsTest, RejectsOutOfRangeUidAndUidByte) {
	write("[tub.first]\n" + tub().replace(tub().find("0x11223344556677"), 16, "0x100000000000000"));
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
	write("[tub.first]\n" + tub().replace(tub().find("0x11223344556677"), 16, "[1, 2, 3, 4, 5, 6, 256]"));
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
}

TEST_F(EventsTest, FillsOmittedTrailingPagesDeterministicallyFromSeed) {
	write("[tub.first]\ntime = 0\nid = 1\nmodule = 1\nuid = 1\npayload = [0x11223344]\n");
	const events::Schedule first      = events::parse_events(path, 42);
	const events::Schedule repeated   = events::parse_events(path, 42);
	const events::Schedule other_seed = events::parse_events(path, 43);
	EXPECT_EQ(first.tubs[0].payload[0], (RfidPage{0x11, 0x22, 0x33, 0x44}));
	EXPECT_EQ(first.tubs[0].payload, repeated.tubs[0].payload);
	EXPECT_NE(first.tubs[0].payload[1], other_seed.tubs[0].payload[1]);
}

TEST_F(EventsTest, RejectsMoreThanThirtyTwoPayloadPages) {
	write("[tub.first]\ntime = 0\nid = 1\nmodule = 1\nuid = 1\npayload = ["
		  "0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]\n");
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
}

TEST_F(EventsTest, RejectsMalformedOrOutOfRangePayloadPages) {
	std::string malformed = tub();
	malformed.replace(malformed.find("payload = [\n    0"), 17, "payload = [\n    [1, 2, 3]");
	write("[tub.first]\n" + malformed);
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);

	std::string oversized = tub();
	oversized.replace(oversized.find("payload = [\n    0"), 17, "payload = [\n    0x100000000");
	write("[tub.first]\n" + oversized);
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
}

TEST_F(EventsTest, RejectsRemovedStructuredFieldsAndLegacyRfid) {
	for (const std::string field : {"origin = [\"module\", 1]", "destination = [\"module\", 2]", "safe = true",
									"needs_security = false", "rfid = []"}) {
		write("[tub.first]\n" + tub(1.5, 3, field + "\n"));
		EXPECT_THROW((void)events::parse_events(path), std::invalid_argument) << field;
	}
}

TEST_F(EventsTest, RejectsUnknownStartingPositionAndInvalidIds) {
	write("[tub.first]\n" + tub(1.5, 3, "position = \"under_the_belt\"\n"));
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
	write("[tub.first]\n" + tub(1.5, 0));
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
	std::string invalid_module = tub();
	invalid_module.replace(invalid_module.find("module = 1"), 10, "module = 25");
	write("[tub.first]\n" + invalid_module);
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
}

TEST_F(EventsTest, AcceptsAScenarioWithNothingInIt) {
	write("");
	const events::Schedule schedule = events::parse_events(path);
	EXPECT_TRUE(schedule.tubs.empty());
	EXPECT_EQ(schedule.last_tick(), 0u);
}

TEST_F(EventsTest, ParsesPropertyConfigurationWithTheLtlDurationSyntax) {
	write("[scenario]\nduration = \"1h2m3.004s\"\nproperties = \"route.spec\"\n");
	const events::Schedule schedule = events::parse_events(path);
	ASSERT_TRUE(schedule.properties.has_value());
	EXPECT_EQ(schedule.properties->duration_tick, 3723004u);
	EXPECT_EQ(schedule.properties->property_file, path.parent_path() / "route.spec");
}

TEST_F(EventsTest, ResolvesScenarioLayoutAndBinaryFromTheScenarioFolder) {
	write("[scenario]\nlayout = \"../layouts/one.toml\"\nfirmware = \"bin/application.dll\"\n");
	const events::ScenarioFiles files = events::parse_scenario_files(path);
	ASSERT_TRUE(files.layout.has_value());
	ASSERT_TRUE(files.firmware.has_value());
	EXPECT_EQ(*files.layout, (path.parent_path() / "../layouts/one.toml").lexically_normal());
	EXPECT_EQ(*files.firmware, (path.parent_path() / "bin/application.dll").lexically_normal());
}

TEST_F(EventsTest, ScenarioLayoutAndBinaryAreOptionalButMustBeNonEmptyStrings) {
	write("");
	const events::ScenarioFiles files = events::parse_scenario_files(path);
	EXPECT_FALSE(files.layout.has_value());
	EXPECT_FALSE(files.firmware.has_value());

	write("[scenario]\nlayout = \"\"\n");
	EXPECT_THROW((void)events::parse_scenario_files(path), std::invalid_argument);
	write("[scenario]\nfirmware = 42\n");
	EXPECT_THROW((void)events::parse_scenario_files(path), std::invalid_argument);
}

TEST_F(EventsTest, RequiresValidPropertyConfiguration) {
	write("[scenario]\nduration = 1.5\nproperties = \"route.spec\"\n");
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
	write("[scenario]\nduration = \"3s\"\n");
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
}

TEST_F(EventsTest, RejectsMissingFileAndRemovedAssertionFormat) {
	EXPECT_THROW((void)events::parse_events(path.string() + ".missing"), std::exception);
	write("[assert.old]\ncheck = \"arrived_at\"\n");
	EXPECT_THROW((void)events::parse_events(path), std::invalid_argument);
}

TEST_F(EventsTest, RejectsRemovedScenarioKeysWithReplacement) {
	for (const char* key : {"binary", "application_path"}) {
		write(std::string("[scenario]\n") + key + "=\"old.exe\"\n");
		try {
			(void)events::parse_scenario_files(path);
			FAIL();
		} catch (const std::invalid_argument& error) {
			EXPECT_NE(std::string(error.what()).find(key), std::string::npos);
			EXPECT_NE(std::string(error.what()).find("firmware"), std::string::npos);
		}
	}
}
