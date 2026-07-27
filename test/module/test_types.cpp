#include <gtest/gtest.h>
#include "../../src/module/types.h"
#include "../../src/defs.h"

TEST(ModuleTypesTest, ModuleId)
{
    EXPECT_THROW(module::ModuleId{0}, std::out_of_range);
    EXPECT_THROW(module::ModuleId{static_cast<uint8_t>(MAX_MODULES)}, std::out_of_range);

    {
        const module::ModuleId mid;
        EXPECT_EQ(0, mid.raw());
        EXPECT_EQ(mid, mid);
    }

    for (std::size_t i = 1; i < MAX_MODULES; i++)
    {
        EXPECT_NO_THROW(module::ModuleId{static_cast<uint8_t>(i)});
        const auto mid = module::ModuleId{static_cast<uint8_t>(i)};
        EXPECT_EQ(mid, mid);
    }
}

TEST(ModuleTypesTest, BeltSpeed)
{
    EXPECT_THROW(module::BeltSpeed{-101}, std::out_of_range);
    EXPECT_THROW(module::BeltSpeed{101}, std::out_of_range);

    const module::BeltSpeed bs;
    EXPECT_DOUBLE_EQ(0, bs.raw());

    EXPECT_NO_THROW(module::BeltSpeed{0});
    EXPECT_NO_THROW(module::BeltSpeed{-100});
    EXPECT_NO_THROW(module::BeltSpeed{100});
    EXPECT_NO_THROW(module::BeltSpeed{75.412423});
}

TEST(ModuleTypesTest, ServoAngle)
{
    EXPECT_THROW(module::ServoAngle{-1}, std::out_of_range);
    EXPECT_THROW(module::ServoAngle{200}, std::out_of_range);

    const module::ServoAngle sa;
    EXPECT_DOUBLE_EQ(0, sa.raw());

    EXPECT_NO_THROW(module::ServoAngle{0});
    EXPECT_NO_THROW(module::ServoAngle{180});
    EXPECT_NO_THROW(module::ServoAngle{75.412423});
}

TEST(ModuleTypesTest, LedColor)
{
    const module::LedColor lc;
    EXPECT_EQ(0, lc.raw());
    EXPECT_EQ(0, lc.r());
    EXPECT_EQ(0, lc.g());
    EXPECT_EQ(0, lc.b());
    EXPECT_EQ(0, lc.a());

    EXPECT_NO_THROW(module::LedColor{0});
    EXPECT_NO_THROW(module::LedColor{0xAABBCC});
}