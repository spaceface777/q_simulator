#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <random>

#include "../src/settings.h"

namespace fs = std::filesystem;

class SettingsTest : public ::testing::Test
{
  protected:
    fs::path temp_settings_path;
    fs::path temp_layout_path;
    fs::path temp_router_path;

    void SetUp() override
    {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::mt19937 rng(static_cast<unsigned int>(now));
        std::uniform_int_distribution<int> dist(100000, 999999);

        temp_settings_path = fs::temp_directory_path() / ("test_settings_" + std::to_string(dist(rng)) + ".toml");
        temp_layout_path = fs::temp_directory_path() / ("test_layout_" + std::to_string(dist(rng)) + ".toml");
        temp_router_path = fs::temp_directory_path() / ("zephyr_" + std::to_string(dist(rng)) + ".exe");

        std::ofstream outfile(temp_router_path);
        outfile << "dummy content";

        write_settings("router_path = \"" + temp_router_path.string() + "\"\n" +
                       R"(
simulation_speed = 1.0
scale = 1.0
)");
        write_layout("");
    }

    void TearDown() override
    {
        if (fs::exists(temp_settings_path))
            fs::remove(temp_settings_path);
        if (fs::exists(temp_layout_path))
            fs::remove(temp_layout_path);
        if (fs::exists(temp_router_path))
            fs::remove(temp_router_path);
    }

    void write_settings(const std::string &content)
    {
        std::ofstream outfile(temp_settings_path);
        outfile << content;
    }

    void write_layout(const std::string &content)
    {
        std::ofstream outfile(temp_layout_path);
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
    EXPECT_THROW(static_cast<void>(settings::get_setting<int>("non_existent_key")), std::invalid_argument);
}

TEST_F(SettingsTest, ParseValidSettings)
{
    write_settings("router_path = \"" + temp_router_path.string() + "\"\n" +
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
}

TEST_F(SettingsTest, ParseSettingsResetsPreviousModuleState)
{
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

TEST_F(SettingsTest, ParseSettingsMissingFile)
{
    TearDown();

    EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsMissingRouterPath)
{
    std::string invalid_toml = R"(simulation_speed = 1.0
scale = 1.0
)";

    write_settings(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsInvalidScale)
{
    std::string invalid_toml = "router_path = \"" + temp_router_path.string() + "\"\n" +
                               R"(simulation_speed = 1.0
scale = 10.0
)";

    write_settings(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsInvalidModuleAngle)
{
    write_layout(R"(
[module.1]
active = true
neighbors = [0, 0, 0]
pos = [0, 0]
angle = 400
)");

    EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::out_of_range);
}

TEST_F(SettingsTest, ParseSettingsInvalidModuleAngle2)
{
    write_layout(R"(
[module.1]
active = true
neighbors = [0, 0, 0]
pos = [0, 0]
angle = 9223372036854775810
)");

    EXPECT_ANY_THROW(settings::parse_settings(temp_settings_path, temp_layout_path));
}

TEST_F(SettingsTest, ParseSettingsInvalidModuleBottom)
{
    write_layout(R"(
[module.1]
active = true
neighbors = [0, 25, 0]
pos = [0, 0]
angle = 0
)");

    EXPECT_THROW(settings::parse_settings(temp_settings_path, temp_layout_path), std::out_of_range);
}

TEST_F(SettingsTest, ParseSettingsOneWayConnectionThrows)
{
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

TEST_F(SettingsTest, ParseSettingsDuplicatePortConnectionThrows)
{
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