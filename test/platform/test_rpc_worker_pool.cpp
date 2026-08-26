#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../../src/engine/task_queue.h"
#include "../../src/platform/rpc_worker_pool.h"

TEST(RpcWorkerPoolTest, DifferentCategoriesStartAndResolveTogether) {
	platform::RpcWorkerPool workers;

	EXPECT_EQ(workers.enqueue(1, platform::Bucket::RFID), 1u);
	EXPECT_EQ(workers.enqueue(2, platform::Bucket::CV_SENSOR), 2u);
	EXPECT_TRUE(workers.is_running(1));
	EXPECT_TRUE(workers.is_running(2));

	EXPECT_TRUE(workers.complete(2).empty());
	EXPECT_FALSE(workers.is_occupied(2));
	EXPECT_TRUE(workers.is_running(1));
	EXPECT_TRUE(workers.complete(1).empty());
}

TEST(RpcWorkerPoolTest, ThreeOccupiedLanesBlockAFourthUntilOneCompletes) {
	platform::RpcWorkerPool workers;

	EXPECT_TRUE(workers.enqueue(1, platform::Bucket::RFID).has_value());
	EXPECT_FALSE(workers.enqueue(2, platform::Bucket::RFID).has_value());
	EXPECT_FALSE(workers.enqueue(3, platform::Bucket::RFID).has_value());
	EXPECT_FALSE(workers.enqueue(4, platform::Bucket::CV_SENSOR).has_value());
	EXPECT_EQ(workers.occupied(), platform::RPC_HANDLER_LANES);
	EXPECT_EQ(workers.queued(), 1u);
	EXPECT_FALSE(workers.is_occupied(4));

	const auto started = workers.complete(1);
	EXPECT_EQ(started, std::vector<uint64_t>({2, 4}));
	EXPECT_TRUE(workers.is_running(4));
	EXPECT_EQ(workers.queued(), 0u);
}

TEST(RpcWorkerPoolTest, RequestsInOneCategoryRemainSerialized) {
	platform::RpcWorkerPool workers;

	EXPECT_TRUE(workers.enqueue(1, platform::Bucket::RFID).has_value());
	EXPECT_FALSE(workers.enqueue(2, platform::Bucket::RFID).has_value());
	EXPECT_FALSE(workers.enqueue(3, platform::Bucket::RFID).has_value());
	EXPECT_TRUE(workers.is_running(1));
	EXPECT_FALSE(workers.is_running(2));
	EXPECT_FALSE(workers.is_running(3));

	EXPECT_EQ(workers.complete(1), std::vector<uint64_t>({2}));

	EXPECT_EQ(workers.complete(2), std::vector<uint64_t>({3}));

	EXPECT_TRUE(workers.complete(3).empty());
}

TEST(RpcWorkerPoolTest, CategorySerializationIsPerModule) {
	platform::RpcWorkerPool module_three;
	platform::RpcWorkerPool module_four;

	EXPECT_EQ(module_three.enqueue(1, platform::Bucket::RFID), 1u);
	EXPECT_EQ(module_four.enqueue(2, platform::Bucket::RFID), 2u);
	EXPECT_TRUE(module_three.is_running(1));
	EXPECT_TRUE(module_four.is_running(2));
}

TEST(RpcWorkerPoolTest, SeededIntakeReplaysTheSameLaneSchedule) {
	const auto run = [](uint64_t seed) {
		platform::RpcWorkerPool workers;
		engine::Queue           queue(seed);
		std::vector<uint64_t>   started;
		for (uint64_t id = 1; id <= 8; id++) {
			queue.schedule(0, engine::Priority::CHILD_INTAKE, "rpc/" + std::to_string(id),
						   [&workers, &started, id](engine::TaskContext&) {
							   if (workers.enqueue(id, static_cast<platform::Bucket>(id)).has_value()) {
								   started.push_back(id);
							   }
						   });
		}
		queue.run_tick(0);

		for (std::size_t index = 0; index < started.size(); index++) {
			const auto next = workers.complete(started[index]);
			started.insert(started.end(), next.begin(), next.end());
		}
		return started;
	};

	EXPECT_EQ(run(73), run(73));
}
