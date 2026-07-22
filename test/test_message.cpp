#include <gtest/gtest.h>
#include "../src/message/message.h"
#include "../src/util.h"

TEST(MessageTest, BaseMessageDefaultBehavior)
{
    message::Message base_msg;
    EXPECT_FALSE(base_msg.is_valid());
    EXPECT_EQ("", base_msg.to_toml());

    auto cloned = base_msg.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_FALSE(cloned->is_valid());
}

TEST(Packet, ToToml)
{
    message::Update update(1, "2", 3);
    const std::string toml = update.to_toml();
    const std::vector<std::string_view> parts = util::split(toml, "\n");

    ASSERT_EQ(4, parts.size());
    ASSERT_STREQ("[update]", std::string(parts[0]).c_str());
    ASSERT_STREQ("module = 1", std::string(parts[1]).c_str());
    ASSERT_STREQ("device = \"2\"", std::string(parts[2]).c_str());
    ASSERT_STREQ("value = 3", std::string(parts[3]).c_str());
}

TEST(Packet, ToTomlPacket)
{
    message::Packet packet(1, 4, "heyy");
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
    const std::string toml = " [init]\n\n id = 4";
    const message::Init intro = message::Init::from_toml(toml).value();

    ASSERT_TRUE(intro.is_valid());
    ASSERT_EQ(4, intro.id());
    ASSERT_STREQ("[init]\nid = 4", intro.to_toml().c_str());
}

TEST(InitTest, ValidationAndCloning)
{
    message::Init init(42);
    EXPECT_TRUE(init.is_valid());
    EXPECT_EQ(42, init.id());

    auto cloned = init.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_EQ(init.to_toml(), cloned->to_toml());
}

TEST(InitTest, FromTomlInvalidInput)
{
    EXPECT_FALSE(message::Init::from_toml("id = 1").has_value());
    EXPECT_FALSE(message::Init::from_toml("[init]\nid = abc").has_value());
    EXPECT_FALSE(message::Init::from_toml("[init]\nid 10").has_value());
    EXPECT_FALSE(message::Init::from_toml("[init]\nfoo = 1").has_value());
}

TEST(InitFinishTest, ToTomlAndFromTomlRoundTrip)
{
    message::InitFinish original(12);
    EXPECT_TRUE(original.is_valid());
    EXPECT_EQ(12, original.id());

    std::string toml = original.to_toml();
    EXPECT_STREQ("[init.finish]\nid = 12", toml.c_str());

    auto parsed = message::InitFinish::from_toml(toml);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(12, parsed->id());
}

TEST(InitFinishTest, FromTomlInvalidInput)
{
    EXPECT_FALSE(message::InitFinish::from_toml("[init]\nid = 12").has_value());
    EXPECT_FALSE(message::InitFinish::from_toml("[init.finish]\nid = \"12\"").has_value());
}

TEST(UpdateTest, ValidityCheck)
{
    message::Update valid_update(1, "sensor_temp", 25);
    EXPECT_TRUE(valid_update.is_valid());

    message::Update invalid_update(1, "", 25);
    EXPECT_FALSE(invalid_update.is_valid());
}

TEST(UpdateTest, FromTomlParsing)
{
    const std::string toml = R"(
[update]
module = 3
device = "LED"
value = 1
)";

    auto parsed = message::Update::from_toml(toml);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(3, parsed->module());
    EXPECT_STREQ("LED", parsed->device().c_str());
    EXPECT_EQ(1, parsed->value());
}

TEST(UpdateTest, FromTomlInvalidInput)
{
    // Invalid integer conversion for module
    std::string bad_module = "[update]\nmodule = bad\ndevice = \"test\"\nvalue = 1";
    EXPECT_FALSE(message::Update::from_toml(bad_module).has_value());

    // Invalid integer conversion for value
    std::string bad_val = "[update]\nmodule = 1\ndevice = \"test\"\nvalue = bad";
    EXPECT_FALSE(message::Update::from_toml(bad_val).has_value());
}

TEST(UpdateRfidTest, ValidityAndSerialization)
{
    message::UpdateRfid valid(1, "UID_1234", "DATA_ABCD");
    EXPECT_TRUE(valid.is_valid());

    message::UpdateRfid empty_uid(1, "", "DATA_ABCD");
    EXPECT_FALSE(empty_uid.is_valid());

    message::UpdateRfid empty_data(1, "UID_1234", "");
    EXPECT_FALSE(empty_data.is_valid());

    std::string toml = valid.to_toml();
    EXPECT_NE(toml.find("[update.rfid]"), std::string::npos);
    EXPECT_NE(toml.find("uid = \"UID_1234\""), std::string::npos);
    EXPECT_NE(toml.find("data = \"DATA_ABCD\""), std::string::npos);
}

TEST(PacketTest, Validity)
{
    // Content length matches length property
    message::Packet valid_packet(1, 5, "hello");
    EXPECT_TRUE(valid_packet.is_valid());

    // Content length mismatch
    message::Packet invalid_packet(1, 10, "hello");
    EXPECT_FALSE(invalid_packet.is_valid());
}

TEST(PacketTest, FromTomlParsing)
{
    const std::string toml = R"(
[packet]
module = 2
length = 4
content = "test"
)";

    auto parsed = message::Packet::from_toml(toml);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(2, parsed->module());
    EXPECT_TRUE(parsed->is_valid());
}

TEST(PacketTest, FromTomlInvalid)
{
    std::string bad_length = "[packet]\nmodule = 1\nlength = string\ncontent = \"test\"";
    EXPECT_FALSE(message::Packet::from_toml(bad_length).has_value());
}

TEST(TubScheduleTest, ToToml)
{
    message::TubSchedule schedule(1, 2, 3, true, false);
    EXPECT_TRUE(schedule.is_valid());

    std::string toml = schedule.to_toml();
    EXPECT_NE(toml.find("[tub.schedule]"), std::string::npos);
    EXPECT_NE(toml.find("destination = 3"), std::string::npos);
    EXPECT_NE(toml.find("safe = 1"), std::string::npos);
    EXPECT_NE(toml.find("needs_security = 0"), std::string::npos);
}

TEST(PlaneArrivalTest, ToToml)
{
    message::PlaneArrival plane(5, 1, 120);
    EXPECT_TRUE(plane.is_valid());

    std::string toml = plane.to_toml();
    EXPECT_NE(toml.find("[plane.arrival]"), std::string::npos);
    EXPECT_NE(toml.find("leaves_in = 120"), std::string::npos);
}

TEST(PlaneDepartureTest, ToToml)
{
    message::PlaneDeparture plane(5, 1);
    EXPECT_TRUE(plane.is_valid());

    std::string toml = plane.to_toml();
    EXPECT_NE(toml.find("[plane.departure]"), std::string::npos);
    EXPECT_NE(toml.find("module = 1"), std::string::npos);
}
