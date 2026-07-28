#include <gtest/gtest.h>
#include "../../src/module/module.h"

class ModuleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        module::clear();
    }

    void TearDown() override
    {
        module::clear();
    }
};

TEST_F(ModuleTest, RegisterAndRetrieveModule)
{
    const uint8_t mod_id = 1;
    auto mod_ptr = std::make_unique<module::Module>(mod_id);

    module::add_module(std::move(mod_ptr));

    auto retrieved = module::get_module(module::ModuleId{mod_id});
    ASSERT_TRUE(retrieved.has_value());
    EXPECT_EQ((*retrieved)->id().raw(), mod_id);
}

TEST_F(ModuleTest, GetNonExistentModuleReturnsEmptyOptional)
{
    auto retrieved = module::get_module(module::ModuleId{20});
    EXPECT_FALSE(retrieved.has_value());
}

TEST_F(ModuleTest, DoubleRegistrationThrowsException)
{
    module::add_module(std::make_unique<module::Module>(1));

    EXPECT_THROW(
        module::add_module(std::make_unique<module::Module>(1)),
        std::invalid_argument);
}

TEST_F(ModuleTest, GetAllModules)
{
    module::add_module(std::make_unique<module::Module>(1));
    module::add_module(std::make_unique<module::Module>(2));

    auto all_modules = module::get_modules();

    bool found_1 = false;
    bool found_2 = false;

    for (const auto &mod : all_modules)
    {
        if (mod->id().raw() == 1)
            found_1 = true;
        if (mod->id().raw() == 2)
            found_2 = true;
    }

    EXPECT_TRUE(found_1);
    EXPECT_TRUE(found_2);
}

TEST_F(ModuleTest, ConcurrentRegistrationThreadSafety)
{
    static constexpr int num_threads = 10;
    std::vector<std::thread> threads;

    for (uint8_t i = 1; i < 1 + num_threads; ++i)
    {
        threads.emplace_back([i]()
                             { module::add_module(std::make_unique<module::Module>(i)); });
    }

    for (auto &t : threads)
    {
        if (t.joinable())
        {
            t.join();
        }
    }

    for (uint8_t i = 1; i < 1 + num_threads; ++i)
    {
        auto mod = module::get_module(module::ModuleId{i});
        EXPECT_TRUE(mod.has_value());
    }
}