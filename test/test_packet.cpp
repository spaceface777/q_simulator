#include <gtest/gtest.h>
#include "../src/packet/packet.h"
#include "../src/util.h"

TEST(Packet, ToToml)
{
    packet::ModuleUpdate update(1, "2", 3);
    const std::string toml = update.to_toml();
    const std::vector<std::string_view> parts = util::split(toml, "\n");

    ASSERT_EQ(4, parts.size());
    ASSERT_STREQ("[update]", std::string(parts[0]).c_str());
    ASSERT_STREQ("id = 1", std::string(parts[1]).c_str());
    ASSERT_STREQ("device = \"2\"", std::string(parts[2]).c_str());
    ASSERT_STREQ("value = 3", std::string(parts[3]).c_str());
}

TEST(Packet, ToTomlPacket)
{
    packet::ModulePacket packet(1, 4, "heyy");
    const std::string toml = packet.to_toml();
    const std::vector<std::string_view> parts = util::split(toml, "\n");

    ASSERT_EQ(4, parts.size());
    ASSERT_STREQ("[packet]", std::string(parts[0]).c_str());
    ASSERT_STREQ("module = 1", std::string(parts[1]).c_str());
    ASSERT_STREQ("length = 4", std::string(parts[2]).c_str());
    ASSERT_STREQ("content = \"heyy\"", std::string(parts[3]).c_str());
}

TEST(Packet, FromTo)
{
    const std::string toml = "  [introduction]\n\n  id  =  4";
    const packet::Introduction intro = packet::Introduction::from_toml(toml).value();

    ASSERT_TRUE(intro.is_valid());
    ASSERT_EQ(4, intro.id());
    ASSERT_STREQ("[introduction]\nid = 4", intro.to_toml().c_str());
}
