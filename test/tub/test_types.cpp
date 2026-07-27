#include <gtest/gtest.h>
#include "../../src/tub/types.h"
#include <limits>

TEST(TubTypesTest, TubId)
{
    EXPECT_THROW(tub::TubId{0}, std::out_of_range);

    {
        const tub::TubId mid;
        EXPECT_EQ(0, mid.raw());
        EXPECT_EQ(mid, mid);
    }

    for (std::size_t i = 1; i < std::numeric_limits<uint8_t>::max(); i++)
    {
        EXPECT_NO_THROW(tub::TubId{static_cast<uint8_t>(i)});
        const auto mid = tub::TubId{static_cast<uint8_t>(i)};
        EXPECT_EQ(mid, mid);
    }
}

TEST(TubTypesTest, LocationProgress)
{
    EXPECT_NO_THROW(tub::LocationProgress{0});
    EXPECT_NO_THROW(tub::LocationProgress{-0.001});
    EXPECT_NO_THROW(tub::LocationProgress{1.0});
    EXPECT_NO_THROW(tub::LocationProgress{1.001});

    EXPECT_THROW(tub::LocationProgress{-1}, std::out_of_range);
    EXPECT_THROW(tub::LocationProgress{2}, std::out_of_range);

    {
        const tub::LocationProgress mid;
        EXPECT_EQ(0, mid.raw());
        EXPECT_EQ(mid, mid);
    }
}