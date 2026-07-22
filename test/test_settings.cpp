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
        "router_path = \"" + temp_router_path.string() +
        "\"\n"
        "simulation_speed = 1.5\n"
        "scale = 2.0\n\n"
        "[module.1]\n"
        "active = true\n"
        "left = 10\n"
        "right = 20\n"
        "bottom = 30\n"
        "angle = 180\n\n"
        "[module.2]\n"
        "active = false\n"
        "left = 0\n"
        "right = 0\n"
        "bottom = 0\n"
        "angle = 90\n";

    write_toml(valid_toml);

    EXPECT_NO_THROW(settings::parse_settings(temp_settings_path.string()));

    EXPECT_DOUBLE_EQ(1.5, settings::get_setting<double>("simulation_speed"));
    EXPECT_DOUBLE_EQ(2.0, settings::get_setting<double>("scale"));
    EXPECT_FALSE(settings::get_setting<bool>("debug"));

    settings::ModuleSettings mod1 = settings::get_module_settings(1);
    EXPECT_TRUE(mod1.active);
    EXPECT_EQ(10, mod1.left);
    EXPECT_EQ(20, mod1.right);
    EXPECT_EQ(30, mod1.bottom);
    EXPECT_EQ(180, mod1.angle);

    EXPECT_EQ(1, settings::get_modules_active());
}

TEST_F(SettingsTest, ParseSettingsMissingFile)
{
    EXPECT_THROW(settings::parse_settings(temp_settings_path.string()), toml::parse_error);
}

TEST_F(SettingsTest, ParseSettingsMissingRouterPath)
{
    std::string invalid_toml =
        "simulation_speed = 1.0\n"
        "scale = 1.0\n";

    write_toml(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path.string()), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsInvalidScale)
{
    std::string invalid_toml =
        "router_path = \"" + temp_router_path.string() +
        "\"\n"
        "simulation_speed = 1.0\n"
        "scale = 10.0\n";

    write_toml(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path.string()), std::invalid_argument);
}

TEST_F(SettingsTest, ParseSettingsInvalidModuleAngle)
{
    std::string invalid_toml =
        "router_path = \"" + temp_router_path.string() +
        "\"\n"
        "simulation_speed = 1.0\n"
        "scale = 1.0\n\n"
        "[module.1]\n"
        "active = true\n"
        "angle = 400\n";

    write_toml(invalid_toml);
    EXPECT_THROW(settings::parse_settings(temp_settings_path.string()), std::invalid_argument);
}