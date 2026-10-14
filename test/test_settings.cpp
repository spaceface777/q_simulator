#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>

#include "../src/settings.h"
#include "../src/world/calibration.h"

namespace fs = std::filesystem;

class SettingsTest : public ::testing::Test {
protected:
	fs::path temp_settings_path;
	fs::path temp_layout_path;
	fs::path temp_application_path;

	void SetUp() override {
		auto                               now = std::chrono::steady_clock::now().time_since_epoch().count();
		std::mt19937                       rng(static_cast<unsigned int>(now));
		std::uniform_int_distribution<int> dist(100000, 999999);

		temp_settings_path    = fs::temp_directory_path() / ("test_settings_" + std::to_string(dist(rng)) + ".toml");
		temp_layout_path      = fs::temp_directory_path() / ("test_layout_" + std::to_string(dist(rng)) + ".toml");
		temp_application_path = fs::temp_directory_path() / ("zephyr_" + std::to_string(dist(rng)) + ".dll");

		std::ofstream outfile(temp_application_path);
		outfile << "dummy content";

		write_settings("firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
					   R"(
simulation_speed = 1.0
scale = 1.0
)");
		write_layout("");
	}

	void TearDown() override {
		if (fs::exists(temp_settings_path)) fs::remove(temp_settings_path);
		if (fs::exists(temp_layout_path)) fs::remove(temp_layout_path);
		if (fs::exists(temp_application_path)) fs::remove(temp_application_path);
	}

	void write_settings(const std::string& content) {
		std::ofstream outfile(temp_settings_path);
		outfile << content;
	}

	void write_layout(const std::string& content) {
		std::ofstream outfile(temp_layout_path);
		outfile << content;
	}
};

TEST_F(SettingsTest, SetAndGetSettings) {
	settings::set_setting<double>("simulation_speed", 2.5);
	EXPECT_DOUBLE_EQ(2.5, settings::get_setting<double>("simulation_speed"));

	settings::set_setting<bool>("debug", true);
	EXPECT_TRUE(settings::get_setting<bool>("debug"));
}

TEST_F(SettingsTest, GetNonExistentSettingThrows) {
	EXPECT_THROW(static_cast<void>(settings::get_setting<int>("non_existent_key")), std::invalid_argument);
}

TEST_F(SettingsTest, ParseValidSettings) {
	write_settings("firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
				   R"(
simulation_speed = 1.5
scale = 2.0
)");

	write_layout(R"(
[module.1]
active = true
neighbors = [2, 0, 0]
pos = [2, 2]
angle = 180

[module.2]
active = false
neighbors = [0, 0, 1]
pos = [1, 1]
angle = 90
        )");

	EXPECT_NO_THROW(settings::parse_settings(temp_settings_path, temp_layout_path));

	EXPECT_DOUBLE_EQ(1.5, settings::get_setting<double>("simulation_speed"));
	EXPECT_DOUBLE_EQ(2.0, settings::get_setting<double>("scale"));
	EXPECT_FALSE(settings::get_setting<bool>("debug"));

	settings::ModuleSettings mod1 = settings::get_module_settings(module::ModuleId(1));
	EXPECT_TRUE(mod1.active);
	ASSERT_TRUE(mod1.left.has_value());
	EXPECT_EQ(2, mod1.left->raw());
	EXPECT_FALSE(mod1.middle.has_value());
	EXPECT_FALSE(mod1.right.has_value());
	EXPECT_EQ(settings::ModuleAngle::UPSIDE_DOWN, mod1.angle);
	EXPECT_EQ(2, mod1.x);
	EXPECT_EQ(2, mod1.y);

	settings::ModuleSettings mod2 = settings::get_module_settings(module::ModuleId(2));
	EXPECT_FALSE(mod2.active);
	EXPECT_FALSE(mod2.left.has_value());
	EXPECT_FALSE(mod2.middle.has_value());
	ASSERT_TRUE(mod2.right.has_value());
	EXPECT_EQ(1, mod2.right->raw());
	EXPECT_EQ(settings::ModuleAngle::ROTATED_90, mod2.angle);
	EXPECT_EQ(1, mod2.x);
	EXPECT_EQ(1, mod2.y);

	EXPECT_EQ(1, settings::get_modules_active());
	EXPECT_EQ(settings::get_active_modules(), std::vector<module::ModuleId>({module::ModuleId{1}}));
	EXPECT_EQ(settings::get_configured_modules(),
			  std::vector<module::ModuleId>({module::ModuleId{1}, module::ModuleId{2}}));
}

TEST_F(SettingsTest, FirmwareOverrideIsAppliedBeforeSettingsValidation) {
	write_settings("firmware = \"/path/that/does/not/exist\"\nseed = 1\n" +
				   std::string("simulation_speed = 1.0\nscale = 1.0\n"));

	EXPECT_NO_THROW(settings::parse_settings(temp_settings_path, temp_layout_path, temp_application_path));
	EXPECT_EQ(settings::get_setting<std::string>("firmware"), temp_application_path.string());
}

TEST_F(SettingsTest, ParseSettingsAcceptsFiniteHardwarePositionSettings) {
	write_settings("firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
				   R"(
simulation_speed = 1.0
scale = 1.0
left_laser_position_mean = -0.25
left_laser_position_sd = 0.0
right_laser_position_mean = 1.25
right_laser_position_sd = 0.125
rfid_position_mean = 2.0
rfid_position_sd = 0.25
)");

	EXPECT_NO_THROW(settings::parse_settings(temp_settings_path, temp_layout_path));
	EXPECT_DOUBLE_EQ(-0.25, settings::get_setting<double>("left_laser_position_mean"));
	EXPECT_DOUBLE_EQ(0.125, settings::get_setting<double>("right_laser_position_sd"));
	EXPECT_DOUBLE_EQ(2.0, settings::get_setting<double>("rfid_position_mean"));
}

TEST_F(SettingsTest, ParseSettingsUsesCanonicalHardwarePositionDefaults) {
	write_settings("firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
				   R"(
simulation_speed = 1.0
scale = 1.0
)");

	ASSERT_NO_THROW(settings::parse_settings(temp_settings_path, temp_layout_path));
	const world::CalibrationConfig defaults;
	EXPECT_DOUBLE_EQ(defaults.left_laser_position_mean, settings::get_setting<double>("left_laser_position_mean"));
	EXPECT_DOUBLE_EQ(defaults.left_laser_position_sd, settings::get_setting<double>("left_laser_position_sd"));
	EXPECT_DOUBLE_EQ(defaults.right_laser_position_mean, settings::get_setting<double>("right_laser_position_mean"));
	EXPECT_DOUBLE_EQ(defaults.right_laser_position_sd, settings::get_setting<double>("right_laser_position_sd"));
	EXPECT_DOUBLE_EQ(defaults.rfid_position_mean, settings::get_setting<double>("rfid_position_mean"));
	EXPECT_DOUBLE_EQ(defaults.rfid_position_sd, settings::get_setting<double>("rfid_position_sd"));
}

TEST_F(SettingsTest, ParseSettingsUsesHardwareEventDelayDefaults) {
	ASSERT_NO_THROW(settings::parse_settings(temp_settings_path, temp_layout_path));
	EXPECT_EQ(settings::get_setting<int>("laser_event_extra_delay_ms"), 3);
	EXPECT_DOUBLE_EQ(settings::get_setting<double>("rfid_event_extra_delay_mean_ms"), 35.0);
	EXPECT_DOUBLE_EQ(settings::get_setting<double>("rfid_event_extra_delay_sd_ms"), 5.0);
}

TEST_F(SettingsTest, ParseSettingsAcceptsHardwareEventDelayOverrides) {
	write_settings("firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
				   R"(
simulation_speed = 1.0
scale = 1.0
laser_event_extra_delay_ms = 8
rfid_event_extra_delay_mean_ms = 42.5
rfid_event_extra_delay_sd_ms = 1.25
)");

	ASSERT_NO_THROW(settings::parse_settings(temp_settings_path, temp_layout_path));
	EXPECT_EQ(settings::get_setting<int>("laser_event_extra_delay_ms"), 8);
	EXPECT_DOUBLE_EQ(settings::get_setting<double>("rfid_event_extra_delay_mean_ms"), 42.5);
	EXPECT_DOUBLE_EQ(settings::get_setting<double>("rfid_event_extra_delay_sd_ms"), 1.25);
}

TEST_F(SettingsTest, ParseSettingsRejectsNegativeHardwareEventDelay) {
	write_settings("firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
				   R"(
simulation_speed = 1.0
scale = 1.0
rfid_event_extra_delay_sd_ms = -1.0
)");

	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsRejectsNegativeHardwarePositionStandardDeviation) {
	write_settings("firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
				   R"(
simulation_speed = 1.0
scale = 1.0
left_laser_position_mean = 0.8
left_laser_position_sd = -0.01
right_laser_position_mean = 0.2
right_laser_position_sd = 0.02
rfid_position_mean = 0.5
rfid_position_sd = 0.1
)");

	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsRejectsNonfiniteHardwarePositionSetting) {
	write_settings("firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
				   R"(
simulation_speed = 1.0
scale = 1.0
left_laser_position_mean = nan
left_laser_position_sd = 0.02
right_laser_position_mean = 0.2
right_laser_position_sd = 0.02
rfid_position_mean = 0.5
rfid_position_sd = 0.1
)");

	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsRejectsHardwarePositionsThatOverflowWorldArithmetic) {
	write_settings("firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
				   R"(
simulation_speed = 1.0
scale = 1.0
left_laser_position_mean = 1e100
left_laser_position_sd = 0.02
right_laser_position_mean = 0.1
right_laser_position_sd = 0.02
rfid_position_mean = 0.6
rfid_position_sd = 0.1
)");

	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsResetsPreviousModuleState) {
	write_layout(R"(
[module.1]
active = true
neighbors = [0, 0, 0]
pos = [1, 1]
angle = 0
        )");

	EXPECT_NO_THROW(settings::parse_settings(temp_settings_path, temp_layout_path));
	EXPECT_EQ(1, settings::get_modules_active());

	write_layout("");

	EXPECT_NO_THROW(settings::parse_settings(temp_settings_path, temp_layout_path));
	EXPECT_EQ(0, settings::get_modules_active());
}

TEST_F(SettingsTest, ParseSettingsMissingFile) {
	TearDown();

	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsInvalidScale) {
	std::string invalid_toml = "firmware = \"" + temp_application_path.string() + "\"\nseed = 1\n" +
							   R"(simulation_speed = 1.0
scale = 10.0
)";

	write_settings(invalid_toml);
	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsInvalidModuleAngle) {
	write_layout(R"(
[module.1]
active = true
neighbors = [0, 0, 0]
pos = [0, 0]
angle = 400
)");

	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::out_of_range);
}

TEST_F(SettingsTest, ParseSettingsInvalidModuleAngle2) {
	write_layout(R"(
[module.1]
active = true
neighbors = [0, 0, 0]
pos = [0, 0]
angle = 9223372036854775810
)");

	EXPECT_ANY_THROW(settings::parse_settings(temp_settings_path, temp_layout_path));
}

TEST_F(SettingsTest, ParseSettingsInvalidModuleBottom) {
	write_layout(R"(
[module.1]
active = true
neighbors = [0, 25, 0]
pos = [0, 0]
angle = 0
)");

	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::out_of_range);
}

TEST_F(SettingsTest, ParseSettingsOneWayConnectionThrows) {
	write_layout(R"(
[module.1]
active = true
neighbors = [0, 0, 2]
pos = [0, 4]
angle = 0

[module.2]
active = true
neighbors = [0, 0, 0]
pos = [1, 4]
angle = 0
)");

	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsDuplicatePortConnectionThrows) {
	write_layout(R"(
        [module.1]
active = true
neighbors = [0, 0, 2]
pos = [0, 4]
angle = 0

[module.2]
active = true
neighbors = [1, 0, 0]
pos = [1, 4]
angle = 0

[module.3]
active = true
neighbors = [2, 0, 0]
pos = [2, 4]
angle = 0
)");

	EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ResolvesRelativeFirmwareFromEachDeclaringFile) {
	write_settings("firmware = \"default.dll\"\nseed=1\nsimulation_speed=1.0\nscale=1.0\n");
	write_layout("[module.1]\nactive=true\nneighbors=[0,0,0]\npos=[0,0]\nangle=0\nfirmware=\"module.dll\"\n");
	settings::parse_settings(temp_settings_path, temp_layout_path);
	EXPECT_EQ(settings::get_setting<std::string>("firmware"),
			  (temp_settings_path.parent_path() / "default.dll").string());
	EXPECT_EQ(settings::get_module_firmware(module::ModuleId{1}),
			  (temp_layout_path.parent_path() / "module.dll").string());
}

TEST_F(SettingsTest, RejectsRemovedSettingsKeysWithReplacement) {
	for (const char* key : {"application_path", "binary"}) {
		write_settings(std::string(key) + "=\"old.exe\"\nseed=1\nsimulation_speed=1.0\nscale=1.0\n");
		try {
			settings::parse_settings(temp_settings_path, temp_layout_path);
			FAIL();
		} catch (const std::invalid_argument& error) {
			EXPECT_NE(std::string(error.what()).find(key), std::string::npos);
			EXPECT_NE(std::string(error.what()).find("firmware"), std::string::npos);
		}
	}
}

TEST_F(SettingsTest, RejectsRemovedModuleBinaryWithReplacement) {
	write_layout("[module.1]\nactive=true\nneighbors=[0,0,0]\npos=[0,0]\nangle=0\nbinary=\"old.exe\"\n");
	try {
		settings::parse_settings(temp_settings_path, temp_layout_path);
		FAIL();
	} catch (const std::invalid_argument& error) {
		EXPECT_NE(std::string(error.what()).find("Module 1"), std::string::npos);
		EXPECT_NE(std::string(error.what()).find("firmware"), std::string::npos);
	}
}
