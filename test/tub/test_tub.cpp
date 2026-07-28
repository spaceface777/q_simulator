#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <chrono>
#include <array>
#include <vector>
#include <functional>

#include "../../src/tub/tub.h"
#include "../../src/module/module.h"
#include "../../src/settings.h"

class TubTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        tub::stop();
        module::clear();
    }

    void TearDown() override
    {
        tub::stop();
        module::clear();
    }

    // Helper to generate empty/default RFID content data
    std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> create_default_rfid()
    {
        std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> rfid{};
        for (auto &block : rfid)
        {
            block.fill(0);
        }
        return rfid;
    }

    void run_intra_module_integration_test(
        ModuleLocation initial_location,
        ModuleLocation target_location,
        module::BeltSpeed small_belt_speed,
        module::BeltSpeed big_belt_speed,
        module::ServoAngle servo_angle,
        std::function<bool(const module::Module &)> is_target_active,
        std::function<bool(const module::Module &)> is_other_active)
    {
        // setup
        settings::set_setting("simulation_speed", 5.0);
        settings::set_module_settings(module::ModuleId{1}, {true, {}, {}, {}, settings::ModuleAngle::NORMAL, 0, 0});

        {
            auto module = std::make_unique<module::Module>(1);
            module->set_small_belt_speed(small_belt_speed);
            module->set_big_belt_speed(big_belt_speed);
            module->set_servo_angle(servo_angle);
            module::add_module(std::move(module));
        }
        {
            auto tub = std::make_unique<tub::Tub>(tub::TubId{1}, module::ModuleId{1}, initial_location, module::ModuleId{1}, initial_location, create_default_rfid());
            tub->run();
            tub::add_tub(std::move(tub));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        const auto module = module::get_module(module::ModuleId{1});
        ASSERT_TRUE(module);
        const auto tub = tub::get_tub(tub::TubId{1});
        ASSERT_TRUE(tub);
        EXPECT_EQ(initial_location, (*tub)->module_location());

        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        EXPECT_LE((*tub)->module_location_progress().raw(), 0.4);
        EXPECT_EQ(initial_location, (*tub)->module_location());

        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        EXPECT_GE((*tub)->module_location_progress().raw(), 0.1);
        EXPECT_EQ(target_location, (*tub)->module_location());

        // exact arrival time unknown so guess
        bool checked = false;
        for (size_t i = 0; i < 20; ++i)
        {
            if (is_target_active(**module))
            {
                checked = true;
                break;
            }

            EXPECT_FALSE(is_other_active(**module));

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        EXPECT_TRUE(checked);
    }
};

TEST_F(TubTest, InitializationAndGetters)
{
    tub::TubId tub_id{42};
    module::ModuleId initial_module{1};
    module::ModuleId target_module{5};
    ModuleLocation target_location = ModuleLocation::LEFT;

    tub::Tub tub(tub_id, initial_module, ModuleLocation::MIDDLE, target_module, target_location, create_default_rfid());

    EXPECT_EQ(tub.id(), tub_id);
    EXPECT_EQ(tub.module(), initial_module);
    EXPECT_EQ(tub.target_module(), target_module);
    EXPECT_EQ(tub.target_module_location(), target_location);
    EXPECT_EQ(tub.module_location(), ModuleLocation::MIDDLE);
    EXPECT_DOUBLE_EQ(tub.module_location_progress().raw(), 0.5);
}

TEST_F(TubTest, AddAndGetTub)
{
    tub::TubId tub_id{10};

    auto tub_ptr = std::make_unique<tub::Tub>(tub_id, module::ModuleId{1}, ModuleLocation::MIDDLE, module::ModuleId{2}, ModuleLocation::RIGHT, create_default_rfid());
    tub::add_tub(std::move(tub_ptr));

    const auto fetched = tub::get_tub(tub_id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ((*fetched)->id(), tub_id);

    const auto non_existent = tub::get_tub(tub::TubId{99});
    EXPECT_FALSE(non_existent.has_value());
}

TEST_F(TubTest, DuplicateRegistrationThrows)
{
    tub::TubId tub_id{7};

    tub::add_tub(std::make_unique<tub::Tub>(tub_id, module::ModuleId{1}, ModuleLocation::MIDDLE, module::ModuleId{2}, ModuleLocation::LEFT, create_default_rfid()));
    EXPECT_THROW({ add_tub(std::make_unique<tub::Tub>(tub_id, module::ModuleId{3}, ModuleLocation::MIDDLE, module::ModuleId{4}, ModuleLocation::RIGHT, create_default_rfid())); }, std::invalid_argument);
}

TEST_F(TubTest, GetAllTubs)
{
    tub::add_tub(std::make_unique<tub::Tub>(tub::TubId{1}, module::ModuleId{10}, ModuleLocation::MIDDLE, module::ModuleId{20}, ModuleLocation::MIDDLE, create_default_rfid()));
    tub::add_tub(std::make_unique<tub::Tub>(tub::TubId{2}, module::ModuleId{10}, ModuleLocation::MIDDLE, module::ModuleId{20}, ModuleLocation::MIDDLE, create_default_rfid()));

    std::vector<std::shared_ptr<tub::Tub>> tubs = tub::get_tubs();
    EXPECT_EQ(tubs.size(), 2);
}

TEST_F(TubTest, DoubleRunThrowsException)
{
    tub::Tub tub(tub::TubId{1}, module::ModuleId{1}, ModuleLocation::MIDDLE, module::ModuleId{2}, ModuleLocation::MIDDLE, create_default_rfid());
    tub.run();

    // cannot double run a tub
    EXPECT_THROW(tub.run(), std::invalid_argument);

    tub.stop();
    EXPECT_NO_THROW(tub.run());
}

TEST_F(TubTest, SafeDestructionWhileRunning)
{
    EXPECT_NO_THROW({
        tub::Tub tub(tub::TubId{100}, module::ModuleId{1}, ModuleLocation::MIDDLE, module::ModuleId{2}, ModuleLocation::MIDDLE, create_default_rfid());
        // test stop()
    });
}

TEST_F(TubTest, GlobalStopAndJoin)
{
    tub::add_tub(std::make_unique<tub::Tub>(tub::TubId{1}, module::ModuleId{1}, ModuleLocation::MIDDLE, module::ModuleId{2}, ModuleLocation::MIDDLE, create_default_rfid()));
    tub::add_tub(std::make_unique<tub::Tub>(tub::TubId{2}, module::ModuleId{2}, ModuleLocation::MIDDLE, module::ModuleId{3}, ModuleLocation::MIDDLE, create_default_rfid()));

    tub::stop();
}

TEST_F(TubTest, IntraModuleIntegrationMiddleToRight)
{
    run_intra_module_integration_test(
        ModuleLocation::MIDDLE,
        ModuleLocation::RIGHT,
        module::BeltSpeed{-100},
        module::BeltSpeed{-100},
        module::ServoAngle{0},
        [](const auto &m)
        { return m.is_right_laser_active(); },
        [](const auto &m)
        { return m.is_rfid_active() || m.is_left_laser_active(); });
}

TEST_F(TubTest, IntraModuleIntegrationMiddleToLeft)
{
    run_intra_module_integration_test(
        ModuleLocation::MIDDLE,
        ModuleLocation::LEFT,
        module::BeltSpeed{-100},
        module::BeltSpeed{100},
        module::ServoAngle{0},
        [](const auto &m)
        { return m.is_left_laser_active(); },
        [](const auto &m)
        { return m.is_rfid_active() || m.is_right_laser_active(); });
}

TEST_F(TubTest, IntraModuleIntegrationLeftToMiddle)
{
    run_intra_module_integration_test(
        ModuleLocation::LEFT,
        ModuleLocation::MIDDLE,
        module::BeltSpeed{100},
        module::BeltSpeed{-100},
        module::ServoAngle{90},
        [](const auto &m)
        { return m.is_rfid_active(); },
        [](const auto &m)
        { return m.is_left_laser_active() || m.is_right_laser_active(); });
}

TEST_F(TubTest, IntraModuleIntegrationRightToMiddle)
{
    run_intra_module_integration_test(
        ModuleLocation::RIGHT,
        ModuleLocation::MIDDLE,
        module::BeltSpeed{100},
        module::BeltSpeed{100},
        module::ServoAngle{90},
        [](const auto &m)
        { return m.is_rfid_active(); },
        [](const auto &m)
        { return m.is_left_laser_active() || m.is_right_laser_active(); });
}

TEST_F(TubTest, IntraModuleIntegrationRightToLeft)
{
    run_intra_module_integration_test(
        ModuleLocation::RIGHT,
        ModuleLocation::LEFT,
        module::BeltSpeed{0},
        module::BeltSpeed{100},
        module::ServoAngle{0},
        [](const auto &m)
        { return m.is_left_laser_active(); },
        [](const auto &m)
        { return m.is_rfid_active() || m.is_right_laser_active(); });
}

TEST_F(TubTest, IntraModuleIntegrationLeftToRight)
{
    run_intra_module_integration_test(
        ModuleLocation::LEFT,
        ModuleLocation::RIGHT,
        module::BeltSpeed{0},
        module::BeltSpeed{-100},
        module::ServoAngle{0},
        [](const auto &m)
        { return m.is_right_laser_active(); },
        [](const auto &m)
        { return m.is_rfid_active() || m.is_left_laser_active(); });
}
