#include <gtest/gtest.h>

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

#include <quercus/rpc_wire.h>

#include "../../src/ltl/checker.h"
#include "../../src/ltl/monitor.h"
#include "../../src/ltl/observation.h"
#include "../../src/ltl/parser.h"
#include "../../src/settings.h"
#include "../../src/world/calibration.h"
#include "../../src/world/world.h"

namespace {

	using Blocks = RfidPayload;

	class LtlObservationTest : public ::testing::Test {
	protected:
		static constexpr uint8_t MODULE = 1;
		static constexpr uint8_t TUB    = 67;

		world::State world_;

		void SetUp() override {
			world::set_calibration_override(world::Calibration::nominal());
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
			settings::ModuleSettings active{};
			active.active = true;
			settings::set_module_settings(module::ModuleId{MODULE}, active);
			world_.add_module(module::ModuleId{MODULE});
		}

		void TearDown() override {
			world_.clear();
			world::clear_calibration_seed();
			for (uint8_t id = 1; id < MAX_MODULES; id++) {
				settings::set_module_settings(module::ModuleId{id}, settings::ModuleSettings{});
			}
		}

		ltl::File compile(const std::string& formula) {
			ltl::File file = ltl::parse("prop test = " + formula + ";", "observation.spec");
			ltl::type_check(file, ltl::TypeEnvironment{{MODULE}}, "observation.spec");
			return file;
		}

		bool evaluate(const std::string& formula, const ltl::Observation& observation) {
			ltl::File file = compile(formula);
			return observation.evaluate(*file.properties[0].formula);
		}

		ltl::Verdict observe(ltl::Monitor& monitor, const ltl::Observation& observation) {
			return monitor.observe([&](const ltl::Formula& atom) { return observation.evaluate(atom); },
								   [&](const ltl::Formula& atom) { return observation.actual_value(atom); },
								   [&](const ltl::Value* domain, ltl::ValueType type, const ltl::DomainVisitor& visit) {
									   observation.visit_domain(domain, type, visit);
								   });
		}

		void place(world::Position position = world::Position::ENTRY) {
			world_.place(TUB, module::ModuleId{MODULE}, position, Blocks{});
		}

		std::vector<module::ModuleId> modules() const { return {module::ModuleId{MODULE}}; }
	};

} // namespace

TEST_F(LtlObservationTest, CapturesLogicalTimeAndEveryModuleField) {
	ltl::Observer   observer(modules(), world_);
	module::Module* hardware = world_.module(module::ModuleId{MODULE});
	hardware->set_big_belt_speed(module::BeltSpeed{-40.0});
	hardware->set_small_belt_speed(module::BeltSpeed{25.0});
	hardware->set_servo_angle(module::ServoAngle{0.0});
	hardware->command_servo_angle(module::ServoAngle{60.0}, 60);
	for (int tick = 0; tick < 15; tick++) hardware->advance_servo_travel();
	hardware->set_led_color(module::LedColor{0x123456});
	hardware->set_left_laser_enabled(true);
	hardware->set_right_laser_enabled(true);
	hardware->set_left_laser_blocked(true);
	hardware->set_right_laser_blocked(true);

	const ltl::Observation observation = observer.observe(1234, world_);

	EXPECT_EQ(observation.tick(), 1234u);
	EXPECT_EQ(observation.timestamp(), std::chrono::milliseconds(1234));
	ASSERT_EQ(observation.modules().size(), 1u);
	EXPECT_DOUBLE_EQ(observation.modules().at(MODULE).big_belt_speed, -40.0);
	EXPECT_DOUBLE_EQ(observation.modules().at(MODULE).small_belt_speed, 25.0);
	EXPECT_DOUBLE_EQ(observation.modules().at(MODULE).servo_angle, 60.0);
	EXPECT_DOUBLE_EQ(observation.modules().at(MODULE).servo_physical_angle, 15.0);
	EXPECT_EQ(observation.modules().at(MODULE).led_color, 0x123456u);
	EXPECT_TRUE(observation.modules().at(MODULE).left_laser_enabled);
	EXPECT_TRUE(observation.modules().at(MODULE).right_laser_enabled);
	EXPECT_TRUE(observation.modules().at(MODULE).left_laser_blocked);
	EXPECT_TRUE(observation.modules().at(MODULE).right_laser_blocked);
}

TEST_F(LtlObservationTest, AbsentTubPredicatesAreFalse) {
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation observation = observer.observe(0, world_);

	EXPECT_FALSE(evaluate("present(tub(67))", observation));
	EXPECT_FALSE(evaluate("at_module(tub(67), module(1))", observation));
	EXPECT_FALSE(evaluate("at_position(tub(67), module(1).entry)", observation));
}

TEST_F(LtlObservationTest, NegationTreatsAnAbsentTubPredicateAsFalseRatherThanUnknown) {
	ltl::File              file = compile("!at_module(tub(67), module(1))");
	ltl::Monitor           monitor("test", *file.properties[0].formula);
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation observation = observer.observe(0, world_);

	EXPECT_EQ(monitor.observe([&observation](const ltl::Formula& atom) { return observation.evaluate(atom); }),
			  ltl::Verdict::TRUE);
}

TEST_F(LtlObservationTest, EvaluatesStatePredicatesAndComparisons) {
	place(world::Position::LEFT_LASER);
	module::Module* hardware = world_.module(module::ModuleId{MODULE});
	hardware->set_servo_angle(module::ServoAngle{60.0000005});
	hardware->set_led_color(module::LedColor{0xabcdef});
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation observation = observer.observe(0, world_);

	EXPECT_TRUE(evaluate("present(tub(67))", observation));
	EXPECT_TRUE(evaluate("at_module(tub(67), module(1))", observation));
	EXPECT_TRUE(evaluate("at_position(tub(67), module(1).left_laser)", observation));
	EXPECT_TRUE(evaluate("module(1).servo_angle == 60.0", observation));
	EXPECT_TRUE(evaluate("module(1).servo_angle > 60.0", observation));
	EXPECT_TRUE(evaluate("module(1).led_color == 0xabcdef", observation));
}

TEST_F(LtlObservationTest, EvaluatesAndRendersByteComparisons) {
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation observation = observer.observe(0, world_);

	EXPECT_TRUE(evaluate(R"("\x00\xff" == "\x00\xff")", observation));
	EXPECT_FALSE(evaluate(R"("same" != "same")", observation));

	ltl::File file = compile(R"("\x00\xff" == "different")");
	EXPECT_EQ(observation.actual_value(*file.properties[0].formula), "\"\\x00\\xff\"");
}

TEST_F(LtlObservationTest, ReadsRawTubAndRpcBytesWithGuardedIndexing) {
	Blocks data{};
	data[0] = {0x81, 0x00, 0x34, 0x12};
	RfidUid uid{1, 2, 3, 4, 5, 6, 7};
	world_.place(TUB, module::ModuleId{MODULE}, world::Position::ENTRY, uid, data);
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation observation =
		observer.observe(0, world_, {{100, 0, MODULE, CMD_SEND_PACKET, 1, 0, true, {'Q', 'F', 'B', '2', 0xaa}}});

	EXPECT_TRUE(evaluate("tub(67).data.length == 128", observation));
	EXPECT_TRUE(evaluate("tub(67).uid[6] == 7", observation));
	ltl::File    request_file = compile(R"(forall network_rpc_request in rpc_requests(module(1)):
		network_rpc_request.payload[0:4] == "QFB2"
		&& network_rpc_request.payload[-3:99] == "B2\xaa")");
	ltl::Monitor request_monitor("request slices", *request_file.properties[0].formula);
	EXPECT_EQ(observe(request_monitor, observation), ltl::Verdict::TRUE);
	EXPECT_TRUE(evaluate("((tub(67).data[0] & 0xf0u) >> 4) == 8", observation));
	EXPECT_TRUE(evaluate("(tub(67).data[2] | (tub(67).data[3] << 8)) == 4660", observation));
	EXPECT_FALSE(evaluate("tub(67).data.length > 200 && tub(67).data[200] == 0", observation));
	EXPECT_THROW((void)evaluate("tub(67).data[128] == 0", observation), std::runtime_error);
}

TEST_F(LtlObservationTest, AppliesOrdinaryEmptyDomainQuantifierAndCountRules) {
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation observation = observer.observe(0, world_);

	ltl::File    forall_file = compile("forall tub: Tub: present(tub)");
	ltl::Monitor forall_monitor("forall", *forall_file.properties[0].formula);
	EXPECT_EQ(observe(forall_monitor, observation), ltl::Verdict::TRUE);

	ltl::File    exists_file = compile("exists tub: Tub: present(tub)");
	ltl::Monitor exists_monitor("exists", *exists_file.properties[0].formula);
	EXPECT_EQ(observe(exists_monitor, observation), ltl::Verdict::FALSE);
	EXPECT_TRUE(evaluate("count tub: Tub where (present(tub)) == 0", observation));
}

TEST_F(LtlObservationTest, QuantifiesAndCountsEveryCurrentEntity) {
	place();
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation observation = observer.observe(0, world_);

	ltl::File    owner_file = compile(R"(
forall tub: Tub:
    exists owner: Module:
        at_module(tub, owner)
)");
	ltl::Monitor owner_monitor("owners", *owner_file.properties[0].formula);
	EXPECT_EQ(observe(owner_monitor, observation), ltl::Verdict::TRUE);
	EXPECT_TRUE(evaluate("count tub: Tub where (present(tub)) == 1", observation));
	EXPECT_TRUE(evaluate("count owner: Module where (owner == module(1)) == 1", observation));

	ltl::File    counted_owner_file = compile(R"(
forall tub: Tub:
    count owner: Module where (at_module(tub, owner)) == 1
)");
	ltl::Monitor counted_owner_monitor("counted owners", *counted_owner_file.properties[0].formula);
	EXPECT_EQ(observe(counted_owner_monitor, observation), ltl::Verdict::TRUE);
}

TEST_F(LtlObservationTest, RetainsInitialDataAndGenericRemovalFacts) {
	Blocks data{};
	data[0] = {0x10, 0x20, 0x30, 0x40};
	world_.place(TUB, module::ModuleId{MODULE}, world::Position::ENTRY, RfidUid{}, data);
	ltl::Observer observer(modules(), world_);
	(void)observer.observe(0, world_);
	const RfidPage changed{0xaa, 0xbb, 0xcc, 0xdd};
	ASSERT_TRUE(world_.write_rfid_page(module::ModuleId{MODULE}, 0, changed));
	const ltl::Observation live = observer.observe(1, world_);
	EXPECT_TRUE(evaluate("initial_data(tub(67))[0] == 16 && tub(67).data[0] == 170", live));

	world_.remove(TUB, world::TubRemovalReason::SCHEDULED);
	const ltl::Observation removed = observer.observe(2, world_);
	EXPECT_TRUE(evaluate("exit_cause(tub(67)) == 1", removed));
	EXPECT_TRUE(evaluate("exit_module(tub(67)) == module(1).id", removed));
	EXPECT_TRUE(evaluate("exit_time(tub(67)) == 2", removed));
	EXPECT_TRUE(evaluate("exit_data(tub(67))[0] == 170", removed));
	EXPECT_FALSE(evaluate("physical_exit(tub(67))", removed));
	EXPECT_FALSE(evaluate("physical_exit(tub(67)) && exit_endpoint(tub(67)) == 1", removed));
}

TEST_F(LtlObservationTest, PersistentTubDomainsRetainRemovedIdentities) {
	place();
	ltl::Observer observer(modules(), world_);
	(void)observer.observe(0, world_);
	world_.remove(TUB);
	const ltl::Observation removed = observer.observe(1, world_);

	ltl::File    file = compile("exists tub: Tub: !present(tub)");
	ltl::Monitor monitor("removed", *file.properties[0].formula);
	EXPECT_EQ(observe(monitor, removed), ltl::Verdict::TRUE);
	EXPECT_TRUE(evaluate("count tub: Tub where (!present(tub)) == 1", removed));
}

TEST_F(LtlObservationTest, FrozenBindingsSurviveProgressedTemporalObligations) {
	place();
	ltl::Observer observer(modules(), world_);
	ltl::File     file = compile(R"(
forall tub: Tub:
    appeared(tub) -> (eventually[1ms, 1ms] disappeared(tub))
)");
	ltl::Monitor  monitor("frozen", *file.properties[0].formula);

	const ltl::Observation appeared = observer.observe(0, world_);
	EXPECT_EQ(observe(monitor, appeared), ltl::Verdict::PENDING);
	world_.remove(TUB);
	const ltl::Observation disappeared = observer.observe(1, world_);
	EXPECT_EQ(observe(monitor, disappeared), ltl::Verdict::TRUE);
}

TEST_F(LtlObservationTest, QuantifiersBeneathAlwaysReevaluateGrowingPersistentDomains) {
	ltl::Observer observer(modules(), world_);
	ltl::File     file = compile("always (forall tub: Tub: present(tub))");
	ltl::Monitor  monitor("present", *file.properties[0].formula);

	EXPECT_EQ(observe(monitor, observer.observe(0, world_)), ltl::Verdict::PENDING);
	place();
	EXPECT_EQ(observe(monitor, observer.observe(1, world_)), ltl::Verdict::PENDING);
	world_.remove(TUB);
	EXPECT_EQ(observe(monitor, observer.observe(2, world_)), ltl::Verdict::FALSE);
}

TEST_F(LtlObservationTest, CachedPersistentGroundingsReadEachNewObservation) {
	ltl::Observer   observer(modules(), world_);
	ltl::File       file = compile("always (forall module: Module: module.left_laser_enabled)");
	ltl::Monitor    monitor("enabled", *file.properties[0].formula);
	module::Module* hardware = world_.module(module::ModuleId{MODULE});

	hardware->set_left_laser_enabled(true);
	for (uint64_t tick = 0; tick < 256; tick++) {
		EXPECT_EQ(observe(monitor, observer.observe(tick, world_)), ltl::Verdict::PENDING);
	}

	hardware->set_left_laser_enabled(false);
	EXPECT_EQ(observe(monitor, observer.observe(256, world_)), ltl::Verdict::FALSE);
}

TEST_F(LtlObservationTest, EqualLookingEntitiesKeepIndependentFrozenObligations) {
	place(world::Position::ENTRY);
	world_.place(68, module::ModuleId{MODULE}, world::Position::LEFT_LASER, Blocks{});
	ltl::Observer observer(modules(), world_);
	ltl::File     file = compile(R"(
forall tub: Tub:
    eventually[1ms, 1ms] disappeared(tub)
)");
	ltl::Monitor  monitor("independent", *file.properties[0].formula);

	EXPECT_EQ(observe(monitor, observer.observe(0, world_)), ltl::Verdict::PENDING);
	world_.remove(TUB);
	EXPECT_EQ(observe(monitor, observer.observe(1, world_)), ltl::Verdict::FALSE);
}

TEST_F(LtlObservationTest, ExposesEveryCurrentRpcBoundaryRecordWithoutCollapsingDuplicates) {
	ltl::Observer                observer(modules(), world_);
	std::vector<ltl::RpcRequest> requests = {
		{100, 0, MODULE, CMD_LASER_LEFT_DETECT, 7, 0, true, {0x01}},
		{101, 0, MODULE, CMD_LASER_LEFT_DETECT, 7, 0, true, {0x01}},
	};
	std::vector<ltl::RpcEvent> events      = {{200, 0, MODULE, EVENT_MESSAGE_RECEIVED, {'h', 'i'}}};
	const ltl::Observation     observation = observer.observe(0, world_, std::move(requests), {}, std::move(events));

	ltl::File    request_file = compile(R"(
forall request in rpc_requests(module(1)):
    request.cmd == .rpc_cmd_laser_left_detect
    && request.call_id == 7
    && request.payload == "\x01"
    && request.confirmable
)");
	ltl::Monitor request_monitor("requests", *request_file.properties[0].formula);
	EXPECT_EQ(observe(request_monitor, observation), ltl::Verdict::TRUE);
	EXPECT_TRUE(evaluate(R"(count request in rpc_requests(module(1)) where (
        request.cmd == .rpc_cmd_laser_left_detect
    ) == 2)",
						 observation));
	EXPECT_TRUE(evaluate(R"(count event in rpc_events(module(1)) where (
        event.kind == .rpc_event_message_received && event.payload == "hi"
    ) == 1)",
						 observation));
}

TEST_F(LtlObservationTest, CorrelatedResponsesBeginUndeliveredAndResolveByUniqueRequestIdentity) {
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation request_tick =
		observer.observe(0, world_, {{100, 0, MODULE, CMD_LASER_LEFT_GET, 42, 0, true, {}}});
	ltl::File    file = compile(R"(
forall request in rpc_requests(module(1)):
    eventually[1ms, 1ms] (
        rpc_response_to(request).request.id == request.id
        && rpc_response_to(request).status == .ok
        && rpc_response_to(request).payload == "answer"
    )
)");
	ltl::Monitor monitor("correlation", *file.properties[0].formula);
	EXPECT_EQ(observe(monitor, request_tick), ltl::Verdict::PENDING);
	ltl::File    undelivered_file = compile(R"(forall request in rpc_requests(module(1)):
		rpc_response_to(request).status == .undelivered)");
	ltl::Monitor undelivered_monitor("undelivered", *undelivered_file.properties[0].formula);
	EXPECT_EQ(observe(undelivered_monitor, request_tick), ltl::Verdict::TRUE);

	const ltl::Observation response_tick = observer.observe(
		1, world_, {}, {{100, 100, 1, MODULE, CMD_LASER_LEFT_GET, 42, 0, {'a', 'n', 's', 'w', 'e', 'r'}}});
	EXPECT_EQ(observe(monitor, response_tick), ltl::Verdict::TRUE);
}

TEST_F(LtlObservationTest, ReclaimsCompletedRpcGroundingsWithoutLosingLiveObligations) {
	ltl::Observer observer(modules(), world_);
	ltl::File     file = compile(R"(
always (
    forall request in rpc_requests(module(1)):
        eventually[1ms, 2ms] (rpc_response_to(request).status == .ok)
)
)");
	ltl::Monitor  monitor("grounding lifetime", *file.properties[0].formula);

	for (uint64_t tick = 0; tick < 130; tick++) {
		const uint64_t                id = 1000 + tick;
		std::vector<ltl::RpcResponse> responses;
		if (tick > 0) {
			const uint64_t previous = id - 1;
			responses.push_back(
				{previous, previous, tick, MODULE, CMD_LASER_LEFT_GET, static_cast<uint16_t>(tick - 1), 0, {}});
		}
		const ltl::Observation observation = observer.observe(
			tick, world_, {{id, tick, MODULE, CMD_LASER_LEFT_GET, static_cast<uint16_t>(tick), 0, true, {}}},
			std::move(responses));
		EXPECT_EQ(observe(monitor, observation), ltl::Verdict::PENDING);
	}

	const uint64_t last = 1129;
	EXPECT_EQ(observe(monitor,
					  observer.observe(130, world_, {}, {{last, last, 130, MODULE, CMD_LASER_LEFT_GET, 129, 0, {}}})),
			  ltl::Verdict::PENDING);
	EXPECT_EQ(monitor.finish(), ltl::Verdict::TRUE);
}

TEST_F(LtlObservationTest, CorrelatedDeadlineFailuresNameTheFrozenRequestAndItsFields) {
	ltl::Observer observer(modules(), world_);
	ltl::File     file = compile(R"(
forall request in rpc_requests(module(1)):
    eventually[1ms, 1ms] (rpc_response_to(request).status == .ok)
)");
	ltl::Monitor  monitor("deadline", *file.properties[0].formula);

	const ltl::Observation request_tick =
		observer.observe(0, world_, {{100, 0, MODULE, CMD_LASER_LEFT_GET, 42, 0, true, {}}});
	EXPECT_EQ(observe(monitor, request_tick), ltl::Verdict::PENDING);
	EXPECT_EQ(observe(monitor, observer.observe(1, world_)), ltl::Verdict::FALSE);

	ASSERT_TRUE(monitor.failure().has_value());
	ASSERT_EQ(monitor.failure()->facts.size(), 1u);
	EXPECT_NE(monitor.failure()->facts[0].expression.find("request#100"), std::string::npos);
	ASSERT_TRUE(monitor.failure()->facts[0].actual.has_value());
	EXPECT_NE(monitor.failure()->facts[0].actual->find(".undelivered"), std::string::npos);
	EXPECT_NE(monitor.failure()->facts[0].actual->find("request.id=100"), std::string::npos);
	EXPECT_NE(monitor.failure()->facts[0].actual->find("module=module(1)"), std::string::npos);
	EXPECT_NE(monitor.failure()->facts[0].actual->find("cmd=.rpc_cmd_laser_left_get"), std::string::npos);
	EXPECT_NE(monitor.failure()->facts[0].actual->find("call_id=42"), std::string::npos);
	EXPECT_EQ(monitor.failure()->obligation_created_tick, 0u);
	EXPECT_EQ(monitor.failure()->deadline_tick, 1u);
}

TEST_F(LtlObservationTest, EqualWireCallIdsCannotSatisfyEachOthersResponseObligations) {
	ltl::Observer observer(modules(), world_);
	ltl::File     file = compile(R"(
forall request in rpc_requests(module(1)):
    eventually[1ms, 2ms] (rpc_response_to(request).status == .ok)
)");
	ltl::Monitor  monitor("independent responses", *file.properties[0].formula);

	EXPECT_EQ(observe(monitor, observer.observe(0, world_,
												{{100, 0, MODULE, CMD_LASER_LEFT_GET, 42, 0, true, {}},
												 {101, 0, MODULE, CMD_LASER_LEFT_GET, 42, 0, true, {}}})),
			  ltl::Verdict::PENDING);
	EXPECT_EQ(observe(monitor, observer.observe(1, world_, {}, {{100, 100, 1, MODULE, CMD_LASER_LEFT_GET, 42, 0, {}}})),
			  ltl::Verdict::PENDING);
	EXPECT_EQ(observe(monitor, observer.observe(2, world_)), ltl::Verdict::FALSE);

	ASSERT_TRUE(monitor.failure().has_value());
	ASSERT_FALSE(monitor.failure()->facts.empty());
	EXPECT_NE(monitor.failure()->facts.back().expression.find("request#101"), std::string::npos);
}

TEST_F(LtlObservationTest, RejectsInvalidOrDuplicateRpcCorrelationIdentities) {
	ltl::Observer observer(modules(), world_);
	EXPECT_THROW((void)observer.observe(0, world_, {}, {{100, 100, 0, MODULE, CMD_HEARTBEAT, 7, 0, {}}}),
				 std::logic_error);

	ltl::Observer mismatched(modules(), world_);
	EXPECT_THROW((void)mismatched.observe(0, world_, {{100, 0, MODULE, CMD_HEARTBEAT, 7, 0, true, {}}},
										  {{101, 100, 0, MODULE, CMD_HEARTBEAT, 7, 0, {}}}),
				 std::logic_error);

	ltl::Observer duplicate_event(modules(), world_);
	(void)duplicate_event.observe(0, world_, {}, {}, {{200, 0, MODULE, EVENT_RFID_DETECT, {}}});
	EXPECT_THROW((void)duplicate_event.observe(1, world_, {}, {}, {{200, 1, MODULE, EVENT_RFID_DETECT, {}}}),
				 std::logic_error);
}

TEST_F(LtlObservationTest, ControlledRpcRecordsFailQuietPeriodMultiplicityStatusAndPreconditionProperties) {
	{
		ltl::Observer observer(modules(), world_);
		ltl::File     file = compile(R"(
always (
    (exists event in rpc_events(module(1)):
        event.kind == .rpc_event_laser_left_detect)
    -> (always[1ms, 2ms] !(exists event in rpc_events(module(1)):
        event.kind == .rpc_event_laser_left_detect))
)
)");
		ltl::Monitor  monitor("quiet", *file.properties[0].formula);
		EXPECT_EQ(
			observe(monitor, observer.observe(0, world_, {}, {}, {{200, 0, MODULE, EVENT_LASER_LEFT_DETECT, {}}})),
			ltl::Verdict::PENDING);
		EXPECT_EQ(observe(monitor, observer.observe(1, world_)), ltl::Verdict::PENDING);
		EXPECT_EQ(
			observe(monitor, observer.observe(2, world_, {}, {}, {{201, 2, MODULE, EVENT_LASER_LEFT_DETECT, {}}})),
			ltl::Verdict::FALSE);
	}

	{
		ltl::Observer observer(modules(), world_);
		ltl::File     file = compile(R"(count event in rpc_events(module(1)) where (
            event.kind == .rpc_event_laser_left_detect
        ) <= 1)");
		ltl::Monitor  monitor("multiplicity", *file.properties[0].formula);
		EXPECT_EQ(observe(monitor, observer.observe(0, world_, {}, {},
													{{200, 0, MODULE, EVENT_LASER_LEFT_DETECT, {}},
													 {201, 0, MODULE, EVENT_LASER_LEFT_DETECT, {}}})),
				  ltl::Verdict::FALSE);
	}

	{
		ltl::Observer observer(modules(), world_);
		ltl::File     file = compile(R"(forall response in rpc_responses(module(1)):
            response.status == .ok)");
		ltl::Monitor  monitor("status", *file.properties[0].formula);
		EXPECT_EQ(observe(monitor, observer.observe(0, world_, {{100, 0, MODULE, CMD_HEARTBEAT, 7, 0, true, {}}},
													{{100, 100, 0, MODULE, CMD_HEARTBEAT, 7, -11, {}}})),
				  ltl::Verdict::FALSE);
	}

	{
		ltl::Observer observer(modules(), world_);
		ltl::File     file = compile(R"(forall event in rpc_events(module(1)):
            (event.kind == .rpc_event_laser_left_detect) -> module(1).left_laser_blocked)");
		ltl::Monitor  monitor("precondition", *file.properties[0].formula);
		EXPECT_EQ(
			observe(monitor, observer.observe(0, world_, {}, {}, {{200, 0, MODULE, EVENT_LASER_LEFT_DETECT, {}}})),
			ltl::Verdict::FALSE);
	}
}

TEST_F(LtlObservationTest, StrongNextReactionRejectsSameTickWrongPayloadAndMissingNextObservation) {
	const std::string formula = R"(
forall event in rpc_events(module(1)):
    (event.kind == .rpc_event_message_received)
    -> (next (
        count request in rpc_requests(module(1)) where (
            request.cmd == .rpc_cmd_send_packet && request.payload == "reply"
        ) >= 1
    ))
)";

	{
		ltl::Observer observer(modules(), world_);
		ltl::File     file = compile(formula);
		ltl::Monitor  monitor("same-tick", *file.properties[0].formula);
		EXPECT_EQ(observe(monitor,
						  observer.observe(0, world_,
										   {{100, 0, MODULE, CMD_SEND_PACKET, 7, 0, true, {'r', 'e', 'p', 'l', 'y'}}},
										   {}, {{200, 0, MODULE, EVENT_MESSAGE_RECEIVED, {}}})),
				  ltl::Verdict::PENDING);
		EXPECT_EQ(observe(monitor, observer.observe(1, world_)), ltl::Verdict::FALSE);
	}

	{
		ltl::Observer observer(modules(), world_);
		ltl::File     file = compile(formula);
		ltl::Monitor  monitor("wrong-payload", *file.properties[0].formula);
		EXPECT_EQ(observe(monitor, observer.observe(0, world_, {}, {}, {{200, 0, MODULE, EVENT_MESSAGE_RECEIVED, {}}})),
				  ltl::Verdict::PENDING);
		EXPECT_EQ(observe(monitor,
						  observer.observe(1, world_,
										   {{100, 1, MODULE, CMD_SEND_PACKET, 7, 0, true, {'w', 'r', 'o', 'n', 'g'}}})),
				  ltl::Verdict::FALSE);
	}

	{
		ltl::Observer observer(modules(), world_);
		ltl::File     file = compile(formula);
		ltl::Monitor  monitor("finite-end", *file.properties[0].formula);
		EXPECT_EQ(observe(monitor, observer.observe(0, world_, {}, {}, {{200, 0, MODULE, EVENT_MESSAGE_RECEIVED, {}}})),
				  ltl::Verdict::PENDING);
		// The observation the reaction was due on never arrived, so the trace ended too early to
		// decide rather than showing the reaction missing.
		EXPECT_EQ(monitor.finish(), ltl::Verdict::UNRESOLVED);
	}
}

TEST_F(LtlObservationTest, DistinguishesServoTargetFromPhysicalAngleAndExposesBlockedDetectors) {
	module::Module* hardware = world_.module(module::ModuleId{MODULE});
	hardware->set_servo_angle(module::ServoAngle{0.0});
	hardware->command_servo_angle(module::ServoAngle{60.0}, 60);
	for (int tick = 0; tick < 15; tick++) hardware->advance_servo_travel();
	hardware->set_left_laser_blocked(true);
	hardware->set_right_laser_blocked(false);
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation observation = observer.observe(0, world_);

	EXPECT_TRUE(evaluate("module(1).servo_angle == 60.0", observation));
	EXPECT_TRUE(evaluate("module(1).servo_physical_angle == 15.0", observation));
	EXPECT_TRUE(evaluate("module(1).left_laser_blocked", observation));
	EXPECT_FALSE(evaluate("module(1).right_laser_blocked", observation));
}

TEST_F(LtlObservationTest, FailureFactsIncludeTheActualComparedValue) {
	module::Module* hardware = world_.module(module::ModuleId{MODULE});
	hardware->set_led_color(module::LedColor{0x00ff00});
	ltl::Observer          observer(modules(), world_);
	const ltl::Observation observation = observer.observe(0, world_);
	ltl::File              file        = compile("module(1).led_color == 0xff0000");
	ltl::Monitor           monitor("test", *file.properties[0].formula);

	EXPECT_EQ(monitor.observe([&observation](const ltl::Formula& atom) { return observation.evaluate(atom); },
							  [&observation](const ltl::Formula& atom) { return observation.actual_value(atom); }),
			  ltl::Verdict::FALSE);
	ASSERT_TRUE(monitor.failure().has_value());
	ASSERT_EQ(monitor.failure()->facts.size(), 1u);
	EXPECT_EQ(monitor.failure()->facts[0].actual, "0x00ff00");
}

TEST_F(LtlObservationTest, ProducesStartupEdgesAndNoRepeatedPulses) {
	ltl::Observer observer(modules(), world_);
	place(world::Position::ENTRY);

	const ltl::Observation first = observer.observe(0, world_);
	EXPECT_TRUE(evaluate("appeared(tub(67))", first));
	EXPECT_TRUE(evaluate("entered(tub(67), module(1).entry)", first));

	const ltl::Observation second = observer.observe(1, world_);
	EXPECT_FALSE(evaluate("appeared(tub(67))", second));
	EXPECT_FALSE(evaluate("entered(tub(67), module(1).entry)", second));
}

TEST_F(LtlObservationTest, ProducesRemovalEdges) {
	place(world::Position::ENTRY);
	ltl::Observer observer(modules(), world_);
	(void)observer.observe(0, world_);
	world_.remove(TUB);

	const ltl::Observation removed = observer.observe(1, world_);
	EXPECT_TRUE(evaluate("disappeared(tub(67))", removed));
	EXPECT_TRUE(evaluate("left(tub(67), module(1).entry)", removed));
	EXPECT_FALSE(evaluate("present(tub(67))", removed));
}

TEST_F(LtlObservationTest, RejectsProgrammaticTubIdReuse) {
	place();
	ltl::Observer observer(modules(), world_);
	(void)observer.observe(0, world_);
	world_.remove(TUB);
	(void)observer.observe(1, world_);
	place();

	EXPECT_THROW((void)observer.observe(2, world_), std::logic_error);
}

TEST_F(LtlObservationTest, DetectsChangedFieldsAgainstTheConstructionBaseline) {
	ltl::Observer observer(modules(), world_);
	world_.module(module::ModuleId{MODULE})->set_led_color(module::LedColor{0x010203});

	const ltl::Observation first = observer.observe(0, world_);
	EXPECT_TRUE(evaluate("changed(module(1).led_color)", first));

	const ltl::Observation second = observer.observe(1, world_);
	EXPECT_FALSE(evaluate("changed(module(1).led_color)", second));
}

TEST_F(LtlObservationTest, DistinguishesTargetAndPhysicalServoChangeEvents) {
	ltl::Observer   observer(modules(), world_);
	module::Module* hardware = world_.module(module::ModuleId{MODULE});
	hardware->command_servo_angle(module::ServoAngle{60.0}, 60);

	const ltl::Observation commanded = observer.observe(0, world_);
	EXPECT_TRUE(evaluate("changed(module(1).servo_angle)", commanded));
	EXPECT_FALSE(evaluate("changed(module(1).servo_physical_angle)", commanded));

	hardware->advance_servo_travel();
	const ltl::Observation moving = observer.observe(1, world_);
	EXPECT_FALSE(evaluate("changed(module(1).servo_angle)", moving));
	EXPECT_TRUE(evaluate("changed(module(1).servo_physical_angle)", moving));
}

TEST_F(LtlObservationTest, DrivesTheProgressionMonitor) {
	ltl::File     file = compile("eventually at_module(tub(67), module(1))");
	ltl::Monitor  monitor("test", *file.properties[0].formula);
	ltl::Observer observer(modules(), world_);

	const ltl::Observation absent = observer.observe(0, world_);
	EXPECT_EQ(monitor.observe([&absent](const ltl::Formula& atom) { return absent.evaluate(atom); }),
			  ltl::Verdict::PENDING);

	place();
	const ltl::Observation present = observer.observe(1, world_);
	EXPECT_EQ(monitor.observe([&present](const ltl::Formula& atom) { return present.evaluate(atom); }),
			  ltl::Verdict::TRUE);
}
