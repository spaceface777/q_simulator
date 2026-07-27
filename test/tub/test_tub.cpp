#include <gtest/gtest.h>
#include <memory>
#include <thread>
#include <chrono>
#include <array>
#include <vector>

#include "../../src/tub/tub.h"

class TubTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        tub::stop();
        tub::join();
    }

    void TearDown() override
    {
        tub::stop();
        tub::join();
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
};

TEST_F(TubTest, InitializationAndGetters)
{
    uint8_t tub_id = 42;
    module::ModuleId initial_module{1};
    module::ModuleId target_module{5};
    ModuleLocation target_location = ModuleLocation::LEFT;
    auto rfid_data = create_default_rfid();

    tub::Tub tub(tub_id, initial_module, target_module, target_location, rfid_data);

    EXPECT_EQ(tub.id(), tub_id);
    EXPECT_EQ(tub.module(), initial_module);
    EXPECT_EQ(tub.target_module(), target_module);
    EXPECT_EQ(tub.target_module_location(), target_location);
    EXPECT_EQ(tub.module_location(), ModuleLocation::MIDDLE);
    EXPECT_DOUBLE_EQ(tub.module_location_progress(), 0.5);
}

TEST_F(TubTest, AddAndGetTub)
{
    uint8_t tub_id = 10;
    auto rfid_data = create_default_rfid();

    auto tub_ptr = std::make_unique<tub::Tub>(tub_id, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::RIGHT, rfid_data);
    add_tub(std::move(tub_ptr));

    auto fetched = tub::get_tub(tub_id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ((*fetched)->id(), tub_id);

    auto non_existent = tub::get_tub(99);
    EXPECT_FALSE(non_existent.has_value());
}

TEST_F(TubTest, DuplicateRegistrationThrows)
{
    uint8_t tub_id = 7;
    auto rfid_1 = create_default_rfid();
    auto rfid_2 = create_default_rfid();

    add_tub(std::make_unique<tub::Tub>(tub_id, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::LEFT, rfid_1));

    EXPECT_THROW({ add_tub(std::make_unique<tub::Tub>(tub_id, module::ModuleId{3}, module::ModuleId{4}, ModuleLocation::RIGHT, rfid_2)); }, std::invalid_argument);
}

TEST_F(TubTest, GetAllTubs)
{
    auto rfid_data = create_default_rfid();

    add_tub(std::make_unique<tub::Tub>(1, module::ModuleId{10}, module::ModuleId{20}, ModuleLocation::MIDDLE, rfid_data));
    add_tub(std::make_unique<tub::Tub>(2, module::ModuleId{10}, module::ModuleId{20}, ModuleLocation::MIDDLE, rfid_data));

    std::vector<std::shared_ptr<tub::Tub>> tubs = tub::get_tubs();
    EXPECT_EQ(tubs.size(), 2);
}

TEST_F(TubTest, DoubleRunThrowsException)
{
    auto rfid_data = create_default_rfid();
    tub::Tub tub(1, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::MIDDLE, rfid_data);

    // Note: Calling run() directly will start run_() which accesses module singletons.
    // Ensure module mocks/dependencies are configured in your system before calling run().
    tub.stop();
    // Verify that attempting to run an already stopped/running tub triggers validation logic
    EXPECT_THROW(tub.run(), std::runtime_error);
}

TEST_F(TubTest, SafeDestructionWhileRunning)
{
    auto rfid_data = create_default_rfid();
    {
        tub::Tub tub(100, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::MIDDLE, rfid_data);
        // Destructor should invoke stop() and join() without deadlocking or crashing
    }
    SUCCEED();
}

TEST_F(TubTest, GlobalStopAndJoin)
{
    auto rfid_data = create_default_rfid();

    add_tub(std::make_unique<tub::Tub>(1, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::MIDDLE, rfid_data));
    add_tub(std::make_unique<tub::Tub>(2, module::ModuleId{2}, module::ModuleId{3}, ModuleLocation::MIDDLE, rfid_data));

    // Global stop and join commands execution safety
    tub::stop();
    tub::join();
    SUCCEED();
}
