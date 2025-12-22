#include <gtest/gtest.h>
#include "../src/purple/purple.h"
#include "../src/util.h"

TEST(Purple, ToToml)
{
    purple::Update update(1, "2", 3);
    const std::string toml = update.to_toml();
    const std::vector<std::string_view> parts = util::split(toml, "\n");

    ASSERT_EQ(4, parts.size());
    ASSERT_STREQ("[update]", std::string(parts[0]).c_str());
    ASSERT_STREQ("id = 1", std::string(parts[1]).c_str());
    ASSERT_STREQ("device = \"2\"", std::string(parts[2]).c_str());
    ASSERT_STREQ("value = 3", std::string(parts[3]).c_str());
}
