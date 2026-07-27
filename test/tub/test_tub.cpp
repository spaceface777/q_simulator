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
    }

    void TearDown() override
    {
        tub::stop();
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
    tub::TubId tub_id{42};
    module::ModuleId initial_module{1};
    module::ModuleId target_module{5};
    ModuleLocation target_location = ModuleLocation::LEFT;

    tub::Tub tub(tub_id, initial_module, target_module, target_location, create_default_rfid());

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

    auto tub_ptr = std::make_unique<tub::Tub>(tub_id, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::RIGHT, create_default_rfid());
    add_tub(std::move(tub_ptr));

    const auto fetched = tub::get_tub(tub_id);
    ASSERT_TRUE(fetched.has_value());
    EXPECT_EQ((*fetched)->id(), tub_id);

    const auto non_existent = tub::get_tub(tub::TubId{99});
    EXPECT_FALSE(non_existent.has_value());
}

TEST_F(TubTest, DuplicateRegistrationThrows)
{
    tub::TubId tub_id{7};

    add_tub(std::make_unique<tub::Tub>(tub_id, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::LEFT, create_default_rfid()));
    EXPECT_THROW({ add_tub(std::make_unique<tub::Tub>(tub_id, module::ModuleId{3}, module::ModuleId{4}, ModuleLocation::RIGHT, create_default_rfid())); }, std::invalid_argument);
}

TEST_F(TubTest, GetAllTubs)
{
    add_tub(std::make_unique<tub::Tub>(tub::TubId{1}, module::ModuleId{10}, module::ModuleId{20}, ModuleLocation::MIDDLE, create_default_rfid()));
    add_tub(std::make_unique<tub::Tub>(tub::TubId{2}, module::ModuleId{10}, module::ModuleId{20}, ModuleLocation::MIDDLE, create_default_rfid()));

    std::vector<std::shared_ptr<tub::Tub>> tubs = tub::get_tubs();
    EXPECT_EQ(tubs.size(), 2);
}

TEST_F(TubTest, DoubleRunThrowsException)
{
    tub::Tub tub(tub::TubId{1}, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::MIDDLE, create_default_rfid());
    tub.run();

    // cannot double run a tub
    EXPECT_THROW(tub.run(), std::invalid_argument);

    tub.stop();
    EXPECT_NO_THROW(tub.run());
}

TEST_F(TubTest, SafeDestructionWhileRunning)
{
    EXPECT_NO_THROW({
        tub::Tub tub(tub::TubId{100}, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::MIDDLE, create_default_rfid());
        // test stop()
    });
}

TEST_F(TubTest, GlobalStopAndJoin)
{
    add_tub(std::make_unique<tub::Tub>(tub::TubId{1}, module::ModuleId{1}, module::ModuleId{2}, ModuleLocation::MIDDLE, create_default_rfid()));
    add_tub(std::make_unique<tub::Tub>(tub::TubId{2}, module::ModuleId{2}, module::ModuleId{3}, ModuleLocation::MIDDLE, create_default_rfid()));

    tub::stop();
}
