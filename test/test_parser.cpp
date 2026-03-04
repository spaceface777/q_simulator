#include <gtest/gtest.h>
#include "../src/event/parser.h"

TEST(Parser, ParseAttribute)
{
    parser::Attribute attribute = parser::parse_attribute("id = 1");

    EXPECT_EQ(attribute.name, "id");
    EXPECT_EQ(attribute.value, "1");

    attribute = parser::parse_attribute("id = 1  # hello");

    EXPECT_EQ(attribute.name, "id");
    EXPECT_EQ(attribute.value, "1");

    attribute = parser::parse_attribute("id = \"#1\"");

    EXPECT_EQ(attribute.name, "id");
    EXPECT_EQ(attribute.value, "\"#1\"");

    attribute = parser::parse_attribute("id = [\"hey\", 1] #\"[hey]");

    EXPECT_EQ(attribute.name, "id");
    EXPECT_EQ(attribute.value, "[\"hey\", 1]");
}
