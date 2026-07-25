#include <gtest/gtest.h>
#include <fstream>
#include <filesystem>
#include <random>
#include <chrono>
#include <memory>
#include <list>
#include <toml++/toml.hpp>

#include "../src/event/events.h"
#include "../src/module/types.h"
#include "../src/synchronization/virtual_clock.h"

namespace fs = std::filesystem;

// Mock clock for controlled testing of fire_events
class MockClock : public virtual_clock::ISimulationClock
{
private:
    std::chrono::milliseconds time_{0};
    double speed_{1.0};

public:
    void set_time_ms(int64_t ms) { time_ = std::chrono::milliseconds(ms); }
    void tick() noexcept override {}
    std::chrono::milliseconds get_virtual_time() const noexcept override { return time_; }
    double simulation_speed() const noexcept override { return speed_; }
    void set_simulation_speed(double speed) override { speed_ = speed; }
};

// Fixture for TOML file parsing tests
class EventsFileTest : public ::testing::Test
{
protected:
    fs::path temp_file_path;

    void SetUp() override
    {
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        std::mt19937 rng(static_cast<unsigned int>(now));
        std::uniform_int_distribution<int> dist(100000, 999999);

        temp_file_path = fs::temp_directory_path() / ("test_events_" + std::to_string(dist(rng)) + ".toml");
    }

    void TearDown() override
    {
        if (fs::exists(temp_file_path))
        {
            fs::remove(temp_file_path);
        }
    }

    void write_events_toml(const std::string &content)
    {
        std::ofstream outfile(temp_file_path);
        outfile << content;
        outfile.close();
    }
};

class TestEvent : public events::Event
{
public:
    mutable bool fired = false;

    TestEvent(double time, unsigned int id)
    {
        time_ = time;
        id_ = id;
    }

    void fire() const override
    {
        fired = true;
    }
};

TEST(EventTest, BaseEventGettersAndEquality)
{
    TestEvent e1(2.5, 101);
    TestEvent e2(5.0, 101);
    TestEvent e3(2.5, 102);

    EXPECT_DOUBLE_EQ(2.5, e1.time());
    EXPECT_EQ(101u, e1.id());

    EXPECT_TRUE(e1 == e2);
    EXPECT_FALSE(e1 == e3);
}

TEST(PlaneEventTest, Getters)
{
    std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> dummy_rfid{};
    dummy_rfid[0] = {1, 2, 3, 4};

    events::PlaneEvent plane(10.5, 42, module::ModuleId(3), 120, dummy_rfid);

    EXPECT_DOUBLE_EQ(10.5, plane.time());
    EXPECT_EQ(42, plane.id());
    EXPECT_EQ(3, plane.module().raw());
    EXPECT_EQ(120, plane.leaves_in());
    EXPECT_EQ(1, plane.rfid()[0][0]);
    EXPECT_EQ(4, plane.rfid()[0][3]);
}

TEST(TubEventTest, Getters)
{
    events::TubData origin{events::LocationType::MODULE, module::ModuleId(1)};
    events::TubData dest{events::LocationType::PLANE, module::ModuleId(2)};
    std::array<std::array<int8_t, RFID_BLOCK_SIZE_BYTES>, RFID_CONTENT_BLOCK_COUNT> dummy_rfid{};

    events::TubEvent tub(5.0, 7, origin, dest, true, false, dummy_rfid);

    EXPECT_DOUBLE_EQ(5.0, tub.time());
    EXPECT_EQ(7u, tub.id());
    EXPECT_EQ(events::LocationType::MODULE, tub.origin().type);
    EXPECT_EQ(1, tub.origin().id.raw());
    EXPECT_EQ(events::LocationType::PLANE, tub.destination().type);
    EXPECT_EQ(2, tub.destination().id.raw());
    EXPECT_TRUE(tub.safe());
    EXPECT_FALSE(tub.needs_security());
}

TEST(FireEventsTest, FiresAndRemovesDueEvents)
{
    MockClock clock;
    std::list<std::unique_ptr<events::Event>> event_list;

    auto e1 = std::make_unique<TestEvent>(1.0, 1);
    auto e2 = std::make_unique<TestEvent>(3.0, 2);
    auto e3 = std::make_unique<TestEvent>(5.0, 3);

    TestEvent *ptr1 = e1.get();
    TestEvent *ptr2 = e2.get();
    TestEvent *ptr3 = e3.get();

    event_list.push_back(std::move(e1));
    event_list.push_back(std::move(e2));
    event_list.push_back(std::move(e3));

    clock.set_time_ms(2000);
    events::fire_events(clock, event_list);

    EXPECT_TRUE(ptr1->fired);
    EXPECT_FALSE(ptr2->fired);
    EXPECT_FALSE(ptr3->fired);
    EXPECT_EQ(2u, event_list.size());

    clock.set_time_ms(5000);
    events::fire_events(clock, event_list);

    EXPECT_TRUE(ptr2->fired);
    EXPECT_TRUE(ptr3->fired);
    EXPECT_TRUE(event_list.empty());
}

TEST_F(EventsFileTest, ParseValidEvents)
{
    std::string valid_toml = R"(
[tub.event1]
time = 1.5
id = 100
safe = true
needs_security = false
origin = ["module", 1]
destination = ["plane", 2]
rfid = [[1, 2, 3, 4]]

[plane.event2]
time = 4.0
id = 200
module = 3
leaves_in = 300
rfid = [[5, 6, 7, 8]]
)";

    write_events_toml(valid_toml);

    std::list<std::unique_ptr<events::Event>> parsed_events;
    EXPECT_NO_THROW(events::parse_events(temp_file_path, parsed_events));
    EXPECT_EQ(2u, parsed_events.size());
}

TEST_F(EventsFileTest, ParseMissingFileThrows)
{
    std::list<std::unique_ptr<events::Event>> parsed_events;
    EXPECT_THROW(events::parse_events(temp_file_path, parsed_events), std::invalid_argument);
}

TEST_F(EventsFileTest, ParseInvalidTubMissingFieldsThrows)
{
    std::string invalid_toml = R"(
[tub.bad_event]
time = 1.0
id = 1
# missing origin and destination
)";

    write_events_toml(invalid_toml);

    std::list<std::unique_ptr<events::Event>> parsed_events;
    EXPECT_THROW(events::parse_events(temp_file_path, parsed_events), std::invalid_argument);
}

TEST_F(EventsFileTest, ParseInvalidRfidDimensionsThrows)
{
    std::string invalid_toml = R"(
[plane.bad_rfid]
time = 2.0
id = 5
module = 1
leaves_in = 60
rfid = [[1, 2, 3]] # invalid array
)";

    write_events_toml(invalid_toml);

    std::list<std::unique_ptr<events::Event>> parsed_events;
    EXPECT_THROW(events::parse_events(temp_file_path, parsed_events), std::invalid_argument);
}