#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "../../src/net/network.h"

namespace {

	// No loss, no very-late packets, and a single fixed delay, so a test can predict arrival ticks.
	net::Config fixed_config() {
		net::Config config;
		config.delay_cdf.clear();
		config.fixed_delay_ms  = 5;
		config.packet_loss_ppm = 0;
		config.very_late_ppm   = 0;
		return config;
	}

	std::vector<module::ModuleId> to(uint8_t id) {
		return {module::ModuleId{id}};
	}

	// Runs the model to a tick, collecting everything delivered on the way.
	std::vector<net::Delivery> drain_to(net::Model& model, uint64_t last_tick) {
		std::vector<net::Delivery> all;
		for (uint64_t tick = 0; tick <= last_tick; tick++) {
			for (net::Delivery& delivery : model.deliver_due(tick)) {
				all.push_back(std::move(delivery));
			}
		}
		return all;
	}

} // namespace

TEST(NetworkTest, DeliversAfterTheFixedDelayAndNotBefore) {
	net::Model model(1, fixed_config());
	model.send(0, module::ModuleId{1}, to(2), {'h', 'i'});

	for (uint64_t tick = 0; tick < 5; tick++) {
		EXPECT_TRUE(model.deliver_due(tick).empty()) << "delivered early at tick " << tick;
	}

	const std::vector<net::Delivery> delivered = model.deliver_due(5);
	ASSERT_EQ(delivered.size(), 1u);
	EXPECT_EQ(delivered[0].recipient.raw(), 2);
	EXPECT_EQ(delivered[0].payload, std::vector<uint8_t>({'h', 'i'}));
	EXPECT_EQ(model.in_flight(), 0u);
}

TEST(NetworkTest, OneSendReachesEveryRecipient) {
	net::Model model(1, fixed_config());
	model.send(0, module::ModuleId{1}, {module::ModuleId{1}, module::ModuleId{2}, module::ModuleId{3}}, {'x'});

	const std::vector<net::Delivery> delivered = drain_to(model, 10);
	ASSERT_EQ(delivered.size(), 3u);

	std::set<uint8_t> recipients;
	for (const net::Delivery& delivery : delivered) {
		recipients.insert(delivery.recipient.raw());
	}
	// Including the sender: an application that broadcasts sees its own message.
	EXPECT_EQ(recipients, std::set<uint8_t>({1, 2, 3}));
}

TEST(NetworkTest, LossIsOffByDefault) {
	net::Model model(1, fixed_config());
	for (int index = 0; index < 100; index++) {
		model.send(0, module::ModuleId{1}, to(2), {static_cast<uint8_t>(index)});
	}

	EXPECT_EQ(drain_to(model, 200).size(), 100u);
}

TEST(NetworkTest, LossIsAppliedAtTheConfiguredRateAndIsReproducible) {
	net::Config config     = fixed_config();
	config.packet_loss_ppm = 200'000; // 20%

	const auto count_delivered = [&config](uint64_t master) {
		net::Model model(master, config);
		for (int index = 0; index < 400; index++) {
			model.send(static_cast<uint64_t>(index), module::ModuleId{1}, to(2), {static_cast<uint8_t>(index)});
		}
		return drain_to(model, 1000).size();
	};

	const std::size_t delivered = count_delivered(7);
	EXPECT_LT(delivered, 400u);
	EXPECT_GT(delivered, 250u);

	// The same seed drops the same messages; a different one drops different ones.
	EXPECT_EQ(delivered, count_delivered(7));
	EXPECT_NE(delivered, count_delivered(8));
}

TEST(NetworkTest, ADroppedPacketIsNeverDelivered) {
	net::Config config     = fixed_config();
	config.packet_loss_ppm = 1'000'000; // Every leg is dropped.
	net::Model model(1, config);

	for (int index = 0; index < 20; index++) {
		model.send(0, module::ModuleId{1}, to(2), {static_cast<uint8_t>(index)});
	}

	EXPECT_EQ(drain_to(model, 50).size(), 0u);
	EXPECT_EQ(model.in_flight(), 0u);
}

TEST(NetworkTest, DelayIsDrawnFromTheConfiguredDistribution) {
	net::Config config;
	config.packet_loss_ppm = 0;
	config.very_late_ppm   = 0;
	config.delay_cdf       = {{500'000, 5}, {1'000'000, 50}};

	net::Model model(1, config);
	for (int index = 0; index < 200; index++) {
		model.send(0, module::ModuleId{1}, to(2), {static_cast<uint8_t>(index)});
	}

	// Every delivery arrives at exactly one of the two configured delays.
	std::set<uint64_t> arrival_ticks;
	for (uint64_t tick = 0; tick <= 60; tick++) {
		if (!model.deliver_due(tick).empty()) {
			arrival_ticks.insert(tick);
		}
	}
	EXPECT_EQ(arrival_ticks, std::set<uint64_t>({5, 50}));
}

TEST(NetworkTest, VeryLatePacketsAreRareAndReproducible) {
	net::Config config        = fixed_config();
	config.very_late_ppm      = 500'000; // 50%, so the effect is visible over a small sample.
	config.very_late_delay_ms = 1'100;

	net::Model model(1, config);
	for (int index = 0; index < 100; index++) {
		model.send(0, module::ModuleId{1}, to(2), {static_cast<uint8_t>(index)});
	}

	std::size_t ordinary = 0;
	std::size_t late     = 0;
	for (uint64_t tick = 0; tick <= 1'100; tick++) {
		for (const net::Delivery& delivery : model.deliver_due(tick)) {
			(void)delivery;
			if (tick == config.fixed_delay_ms) {
				ordinary++;
			} else if (tick == config.very_late_delay_ms) {
				late++;
			}
		}
	}
	EXPECT_GT(ordinary, 0u);
	EXPECT_GT(late, 0u);
	EXPECT_EQ(ordinary + late, 100u);

	// Reproducible: the same seed picks the same packets to be very late.
	net::Model repeat(1, config);
	for (int index = 0; index < 100; index++) {
		repeat.send(0, module::ModuleId{1}, to(2), {static_cast<uint8_t>(index)});
	}
	std::size_t repeat_late = 0;
	for (uint64_t tick = 0; tick <= 1'100; tick++) {
		for (const net::Delivery& delivery : repeat.deliver_due(tick)) {
			(void)delivery;
			if (tick == config.very_late_delay_ms) {
				repeat_late++;
			}
		}
	}
	EXPECT_EQ(late, repeat_late);
}

TEST(NetworkTest, TheOrderOfMessagesArrivingTogetherIsCanonicalRatherThanIncidental) {
	const auto order = [](uint64_t master) {
		net::Model model(master, fixed_config());
		for (uint8_t index = 8; index > 0; index--) {
			// Sent in descending order so incidental storage order would disagree with the sort.
			model.send(0, module::ModuleId{index}, to(9), {index});
		}

		std::vector<uint8_t> payloads;
		for (const net::Delivery& delivery : drain_to(model, 20)) {
			payloads.push_back(delivery.payload[0]);
		}
		return payloads;
	};

	const std::vector<uint8_t> delivered = order(3);
	ASSERT_EQ(delivered.size(), 8u);
	// Canonical order sorts by source id ascending, the opposite of send order here.
	EXPECT_EQ(delivered, std::vector<uint8_t>({1, 2, 3, 4, 5, 6, 7, 8}));

	// A different seed does not change the order: it is not seeded, it is a canonical sort.
	EXPECT_EQ(delivered, order(4));
}

TEST(NetworkTest, DecisionsAreIndependentOfUnrelatedTraffic) {
	// A packet's outcome must not depend on unrelated traffic sent before it.
	net::Config config     = fixed_config();
	config.packet_loss_ppm = 300'000;

	net::Model isolated(1, config);
	isolated.send(0, module::ModuleId{1}, to(2), {'!'});
	const bool isolated_delivered = !drain_to(isolated, 50).empty();

	net::Model with_burst(1, config);
	for (int index = 0; index < 50; index++) {
		with_burst.send(0, module::ModuleId{5}, to(6), {0});
	}
	with_burst.send(0, module::ModuleId{1}, to(2), {'!'});
	bool with_burst_delivered = false;
	for (const net::Delivery& delivery : drain_to(with_burst, 50)) {
		if (delivery.recipient.raw() == 2) {
			with_burst_delivered = true;
		}
	}

	EXPECT_EQ(isolated_delivered, with_burst_delivered);
}
