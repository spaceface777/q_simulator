#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <random>
#include <chrono>
#include "../src/settings.h"

namespace fs = std::filesystem;

class SettingsTest : public ::testing::Test
{
protected:
    fs::path temp_settings_path;
    fs::path temp_router_path;

    void SetUp() override
    {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::mt19937 rng(static_cast<unsigned int>(now));
        std::uniform_int_distribution<int> dist(100000, 999999);

        temp_settings_path = fs::temp_directory_path() / ("test_settings_" + std::to_string(dist(rng)) + ".toml");
        temp_router_path = fs::temp_directory_path() / ("zephyr_" + std::to_string(dist(rng)) + ".exe");

        std::ofstream outfile(temp_router_path);
        outfile << "dummy content";
    }

    void TearDown() override
    {
        if (fs::exists(temp_settings_path))
            fs::remove(temp_settings_path);
        if (fs::exists(temp_router_path))
            fs::remove(temp_router_path);
    }

    void write_toml(const std::string &content)
    {
        std::ofstream outfile(temp_settings_path);
        outfile << content;
    }
};

TEST_F(SettingsTest, SetAndGetSettings)
{
    settings::set_setting<double>("simulation_speed", 2.5);
    EXPECT_DOUBLE_EQ(2.5, settings::get_setting<double>("simulation_speed"));

    settings::set_setting<bool>("debug", true);
    EXPECT_TRUE(settings::get_setting<bool>("debug"));
}

TEST_F(SettingsTest, GetNonExistentSettingThrows)
{
    EXPECT_THROW(settings::get_setting<int>("non_existent_key"), std::invalid_argument);
}

TEST_F(SettingsTest, GetModuleSettingsBounds)
{
    EXPECT_THROW(settings::get_module_settings(0), std::invalid_argument);
    EXPECT_THROW(settings::get_module_settings(MAX_MODULES), std::invalid_argument);
}

TEST_F(SettingsTest, ParseValidSettings)
{
    std::string valid_toml =
        "router_path = \"" + temp_router_path.string() + "\"\n" +
        R"(
simulation_speed = 1.5
scale = 2.0

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
)";

    write_toml(valid_toml);

    EXPECT_NO_THROW(settings::parse_settings(temp_settings_path));

    EXPECT_DOUBLE_EQ(1.5, settings::get_setting<double>("simulation_speed"));
    EXPECT_DOUBLE_EQ(2.0, settings::get_setting<double>("scale"));
    EXPECT_FALSE(settings::get_setting<bool>("debug"));

    settings::ModuleSettings mod1 = settings::get_module_settings(1);
    EXPECT_TRUE(mod1.active);
    EXPECT_EQ(2, mod1.left);
    EXPECT_EQ(0, mod1.middle);
    EXPECT_EQ(0, mod1.right);
    EXPECT_EQ(180, mod1.angle);
    EXPECT_EQ(2, mod1.x);
    EXPECT_EQ(2, mod1.y);

    settings::ModuleSettings mod2 = settings::get_module_settings(2);
    EXPECT_FALSE(mod2.active);
    EXPECT_EQ(0, mod2.left);
    EXPECT_EQ(0, mod2.middle);
    EXPECT_EQ(1, mod2.right);
    EXPECT_EQ(90, mod2.angle);
    EXPECT_EQ(1, mod2.x);
    EXPECT_EQ(1, mod2.y);

    EXPECT_EQ(1, settings::get_modules_active());
}

TEST_F(SettingsTest, ParseSettingsResetsPreviousModuleState)
{
    std::string first_toml =
        "router_path = \"" + temp_router_path.string() + "\"\n" +
        R"(
simulation_speed = 1.5
scale = 2.0

[module.1]
active = true
neighbors = [0, 0, 0]
pos = [1, 1]
angle = 0
)";

    write_toml(first_toml);
    EXPECT_NO_THROW(settings::parse_settings(temp_settings_path));
    EXPECT_EQ(1, settings::get_modules_active());

    std::string second_toml =
        "router_path = \"" + temp_router_path.string() + "\"\n" +
        R"(
simulation_speed = 1.0
scale = 1.0
)";

    write_toml(second_toml);
    EXPECT_NO_THROW(settings::parse_settings(temp_settings_path));
    EXPECT_EQ(0, settings::get_modules_active());
}

TEST_F(SettingsTest, ParseSettingsMissingFile)
{
    EXPECT_THROW(settings::parse_settings(temp_settings_path), toml::parse_error);
}

TEST_F(SettingsTest, ParseSettingsMissingRouterPath)
{
    std::string invalid_toml = R"(simulation_speed = 1.0
scale = 1.0
)";

    write_toml(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsInvalidScale)
{
    std::string invalid_toml =
        "router_path = \"" + temp_router_path.string() + "\"\n" +
        R"(simulation_speed = 1.0
scale = 10.0
)";

    write_toml(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsInvalidModuleAngle)
{
    std::string invalid_toml =
        "router_path = \"" + temp_router_path.string() + "\"\n" +
        R"(simulation_speed = 1.0
scale = 1.0

[module.1]
active = true
angle = 400
)";

    write_toml(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsInvalidModuleBottom)
{
    std::string invalid_toml =
        "router_path = \"" + temp_router_path.string() + "\"\n" +
        R"(
simulation_speed = 1.0
scale = 1.0

[module.1]
active = true
neighbors = [0, 25, 0]
pos = [0, 0]
angle = 0
)";

    write_toml(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsOneWayConnectionThrows)
{
    std::string invalid_toml =
        "router_path = \"" + temp_router_path.string() + "\"\n" +
        R"(simulation_speed = 1.0
scale = 1.0

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
)";

    write_toml(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsDuplicatePortConnectionThrows)
{
    std::string invalid_toml =
        "router_path = \"" + temp_router_path.string() + "\"\n" +
        R"(simulation_speed = 1.0
scale = 1.0

[module.1]
active = true
left = 0
right = 2
bottom = 0
pos = [0, 4]
angle = 0

[module.2]
active = true
left = 1
right = 0
bottom = 0
pos = [1, 4]
angle = 0

[module.3]
active = true
left = 2
right = 0
bottom = 0
pos = [2, 4]
angle = 0
)";

    write_toml(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path), std::invalid_argument);
}