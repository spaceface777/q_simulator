#include <gtest/gtest.h>
#include "../../src/message/message.h"
#include "../../src/util.h"
#include "../../src/module/types.h"

TEST(MessageTest, BaseMessageDefaultBehavior)
{
    message::Message base_msg;
    EXPECT_FALSE(base_msg.is_valid());
    EXPECT_EQ("", base_msg.to_toml());

    auto cloned = base_msg.clone();
    ASSERT_NE(cloned, nullptr);
    EXPECT_FALSE(cloned->is_valid());
}

TEST(PacketTest, ToToml)
{
    message::Update update(module::ModuleId(1), "2", 3);
    const std::string toml = update.to_toml();
    const std::vector<std::string_view> parts = util::split(toml, "\n");

    ASSERT_EQ(4, parts.size());
    ASSERT_STREQ("[update]", std::string(parts[0]).c_str());
    ASSERT_STREQ("module = 1", std::string(parts[1]).c_str());
    ASSERT_STREQ("device = \"2\"", std::string(parts[2]).c_str());
    ASSERT_STREQ("value = 3", std::string(parts[3]).c_str());
}

TEST(PacketTest, ToTomlPacket)
{
    message::Packet packet(module::ModuleId(1), "heyy");
    const std::string toml = packet.to_toml();
    const std::vector<std::string_view> parts = util::split(toml, "\n");

    ASSERT_EQ(4, parts.size());
    ASSERT_STREQ("[packet]", std::string(parts[0]).c_str());
    ASSERT_STREQ("module = 1", std::string(parts[1]).c_str());
    ASSERT_STREQ("length = 4", std::string(parts[2]).c_str());
    ASSERT_STREQ("data = \"heyy\"", std::string(parts[3]).c_str());
}

TEST(PacketTest, FromTo)

{
    const std::string toml = " [init]\n\n id = 4";
    const message::Init intro = message::Init::from_toml(toml).value();

    ASSERT_TRUE(intro.is_valid());
    ASSERT_EQ(4, intro.id().raw());
    ASSERT_STREQ("[init]\nid = 4", intro.to_toml().c_str());
}

TEST(InitTest, ValidationAndCloning)
{
    message::Init init(module::ModuleId(1));
    EXPECT_TRUE(init.is_valid());
    EXPECT_EQ(1, init.id().raw());

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
    EXPECT_FALSE(message::Init::from_toml("[init]\nid = 300").has_value());
}

TEST(InitFinishTest, ToTomlAndFromTomlRoundTrip)
{
    message::InitFinish original(module::ModuleId(12));
    EXPECT_TRUE(original.is_valid());
    EXPECT_EQ(12, original.id().raw());

    std::string toml = original.to_toml();
    EXPECT_STREQ("[init.finish]\nid = 12", toml.c_str());

    auto parsed = message::InitFinish::from_toml(toml);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(12, parsed->id().raw());
}

TEST(InitFinishTest, FromTomlInvalidInput)
{
    EXPECT_FALSE(message::InitFinish::from_toml("[init]\nid = 12").has_value());
    EXPECT_FALSE(message::InitFinish::from_toml("[init.finish]\nid = \"12\"").has_value());
}

TEST(UpdateTest, ValidityCheck)
{
    message::Update valid_update(module::ModuleId(1), "sensor_temp", 25);
    EXPECT_TRUE(valid_update.is_valid());

    message::Update invalid_update(module::ModuleId(1), "", 25);
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
    EXPECT_EQ(3, parsed->module().raw());
    EXPECT_STREQ("LED", parsed->device().c_str());
    EXPECT_EQ(1, parsed->value());
}

TEST(UpdateTest, FromTomlInvalidInput)
{
    std::string bad_module = "[update]\nmodule = bad\ndevice = \"test\"\nvalue = 1";
    EXPECT_FALSE(message::Update::from_toml(bad_module).has_value());

    std::string bad_val = "[update]\nmodule = 1\ndevice = \"test\"\nvalue = bad";
    EXPECT_FALSE(message::Update::from_toml(bad_val).has_value());

    std::string bad_module_range = "[update]\nmodule = 300\ndevice = \"test\"\nvalue = 1";
    EXPECT_FALSE(message::Update::from_toml(bad_module_range).has_value());

    std::string bad_device = "[update]\nmodule = 1\ndevice = test\nvalue = 1";
    EXPECT_FALSE(message::Update::from_toml(bad_device).has_value());
}

TEST(UpdateRfidTest, ValidityAndSerialization)
{
    message::UpdateRfid valid(module::ModuleId(1), "UID_1234", "DATA_ABCD");
    EXPECT_TRUE(valid.is_valid());

    message::UpdateRfid empty_uid(module::ModuleId(1), "", "DATA_ABCD");
    EXPECT_FALSE(empty_uid.is_valid());

    message::UpdateRfid empty_data(module::ModuleId(1), "UID_1234", "");
    EXPECT_FALSE(empty_data.is_valid());

    std::string toml = valid.to_toml();
    EXPECT_NE(toml.find("[update.rfid]"), std::string::npos);
    EXPECT_NE(toml.find("uid = \"UID_1234\""), std::string::npos);
    EXPECT_NE(toml.find("data = \"DATA_ABCD\""), std::string::npos);
}

TEST(PacketTest, Validity)
{
    message::Packet valid_packet(module::ModuleId(1), "hello");
    EXPECT_TRUE(valid_packet.is_valid());

    message::Packet invalid_packet(module::ModuleId(1), "");
    EXPECT_FALSE(invalid_packet.is_valid());
}

TEST(PacketTest, FromTomlParsing)
{
    const std::string toml = R"(
[packet]
module = 2
length = 4
data = "test"
)";

    auto parsed = message::Packet::from_toml(toml);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(2, parsed->module().raw());
    EXPECT_TRUE(parsed->is_valid());
}

TEST(PacketTest, FromTomlInvalid)
{
    std::string bad_length = "[packet]\nmodule = 1\nlength = string\ncontent = \"test\"";
    EXPECT_FALSE(message::Packet::from_toml(bad_length).has_value());

    std::string bad_negative_length = "[packet]\nmodule = 1\nlength = -1\ncontent = \"test\"";
    EXPECT_FALSE(message::Packet::from_toml(bad_negative_length).has_value());

    std::string bad_content = "[packet]\nmodule = 1\nlength = 4\ncontent = test";
    EXPECT_FALSE(message::Packet::from_toml(bad_content).has_value());
}

TEST(TubScheduleTest, ToToml)
{
    message::TubSchedule schedule(1, module::ModuleId(2), module::ModuleId(3), true, false);
    EXPECT_TRUE(schedule.is_valid());

    std::string toml = schedule.to_toml();
    EXPECT_NE(toml.find("[tub.schedule]"), std::string::npos);
    EXPECT_NE(toml.find("destination = 3"), std::string::npos);
    EXPECT_NE(toml.find("safe = 1"), std::string::npos);
    EXPECT_NE(toml.find("needs_security = 0"), std::string::npos);
}

TEST(PlaneArrivalTest, ToToml)
{
    message::PlaneArrival plane(5, module::ModuleId(1), 120);
    EXPECT_TRUE(plane.is_valid());

    std::string toml = plane.to_toml();
    EXPECT_NE(toml.find("[plane.arrival]"), std::string::npos);
    EXPECT_NE(toml.find("leaves_in = 120"), std::string::npos);
}

TEST(PlaneDepartureTest, ToToml)
{
    message::PlaneDeparture plane(5, module::ModuleId(1));
    EXPECT_TRUE(plane.is_valid());

    std::string toml = plane.to_toml();
    EXPECT_NE(toml.find("[plane.departure]"), std::string::npos);
    EXPECT_NE(toml.find("module = 1"), std::string::npos);
}

TEST(UpdateGraph, ToToml)
{
    message::UpdateGraph graph(module::ModuleId(1), 2, "1:1,2,3|2:3,4,5");
    EXPECT_TRUE(graph.is_valid());

    std::string toml = graph.to_toml();
    EXPECT_NE(toml.find("[update.graph]"), std::string::npos);
    EXPECT_NE(toml.find("module = 1"), std::string::npos);
    EXPECT_NE(toml.find("size = 2"), std::string::npos);
    EXPECT_NE(toml.find("data_length = 15"), std::string::npos);
    EXPECT_NE(toml.find("data = \"1:1,2,3|2:3,4,5\""), std::string::npos);
}

TEST(UpdateRfidTest, ToToml)
{
    message::UpdateRfid rfid(module::ModuleId(1), "AA", "BBCC");
    EXPECT_TRUE(rfid.is_valid());

    std::string toml = rfid.to_toml();
    EXPECT_NE(toml.find("[update.rfid]"), std::string::npos);
    EXPECT_NE(toml.find("module = 1"), std::string::npos);
    EXPECT_NE(toml.find("uid_length = 2"), std::string::npos);
    EXPECT_NE(toml.find("uid = \"AA\""), std::string::npos);
    EXPECT_NE(toml.find("data_length = 4"), std::string::npos);
    EXPECT_NE(toml.find("data = \"BBCC\""), std::string::npos);
}
