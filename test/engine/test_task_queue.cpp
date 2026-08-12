#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "../../src/engine/task_queue.h"

TEST(TaskQueueTest, RunsTasksInTickOrderAndStopsAtTheTickBoundary) {
	std::vector<std::string> log;
	engine::Queue            queue(1);

	queue.schedule(2, engine::Priority::WORLD_ADVANCE, "later",
				   [&log](engine::TaskContext&) { log.push_back("later"); });
	queue.schedule(1, engine::Priority::WORLD_ADVANCE, "now", [&log](engine::TaskContext&) { log.push_back("now"); });

	queue.run_tick(1);
	EXPECT_EQ(log, std::vector<std::string>({"now"}));

	queue.run_tick(2);
	EXPECT_EQ(log, std::vector<std::string>({"now", "later"}));
	EXPECT_TRUE(queue.empty());
}

TEST(TaskQueueTest, RunsPrioritiesInOrderWithinATick) {
	std::vector<std::string> log;
	engine::Queue            queue(1);

	queue.schedule(0, engine::Priority::TRACE_FINALIZE, "trace",
				   [&log](engine::TaskContext&) { log.push_back("trace"); });
	queue.schedule(0, engine::Priority::WORLD_ADVANCE, "world",
				   [&log](engine::TaskContext&) { log.push_back("world"); });
	queue.schedule(0, engine::Priority::CHILD_INTAKE, "intake",
				   [&log](engine::TaskContext&) { log.push_back("intake"); });
	queue.schedule(0, engine::Priority::MONITORING, "assert",
				   [&log](engine::TaskContext&) { log.push_back("assert"); });

	queue.run_tick(0);
	EXPECT_EQ(log, std::vector<std::string>({"intake", "world", "assert", "trace"}));
}

TEST(TaskQueueTest, RunsWorkCreatedWithinATickAfterTheWorkThatCreatedIt) {
	std::vector<std::string> log;
	engine::Queue            queue(1);

	queue.schedule(0, engine::Priority::CHILD_INTAKE, "first", [&log](engine::TaskContext& context) {
		log.push_back("first");
		// Scheduled at a priority that has already passed. Generations are what
		// stop this from being dropped or from jumping the queue.
		context.schedule_same_tick(engine::Priority::CHILD_INTAKE, "cascade",
								   [&log](engine::TaskContext&) { log.push_back("cascade"); });
	});
	queue.schedule(0, engine::Priority::TRACE_FINALIZE, "trace",
				   [&log](engine::TaskContext&) { log.push_back("trace"); });

	queue.run_tick(0);
	EXPECT_EQ(log, std::vector<std::string>({"first", "trace", "cascade"}));
}

TEST(TaskQueueTest, ReportsTheGenerationOfACascade) {
	std::vector<uint32_t> generations;
	engine::Queue         queue(1);

	queue.schedule(0, engine::Priority::CHILD_INTAKE, "root", [&generations](engine::TaskContext& context) {
		generations.push_back(context.generation());
		context.schedule_same_tick(engine::Priority::CHILD_INTAKE, "child", [&generations](engine::TaskContext& inner) {
			generations.push_back(inner.generation());
			inner.schedule_same_tick(
				engine::Priority::CHILD_INTAKE, "grandchild",
				[&generations](engine::TaskContext& deepest) { generations.push_back(deepest.generation()); });
		});
	});

	queue.run_tick(0);
	EXPECT_EQ(generations, std::vector<uint32_t>({0, 1, 2}));
}

TEST(TaskQueueTest, ScheduledFutureWorkRunsAtItsOwnTick) {
	std::vector<std::string> log;
	engine::Queue            queue(1);

	queue.schedule(0, engine::Priority::CHILD_INTAKE, "request", [&log](engine::TaskContext& context) {
		log.push_back("request");
		context.schedule_future(context.tick() + 2, engine::Priority::PLATFORM_OUTBOUND, "response",
								[&log](engine::TaskContext&) { log.push_back("response"); });
	});

	queue.run_tick(0);
	queue.run_tick(1);
	EXPECT_EQ(log, std::vector<std::string>({"request"}));

	queue.run_tick(2);
	EXPECT_EQ(log, std::vector<std::string>({"request", "response"}));
}

TEST(TaskQueueTest, ShufflesWithinABucketReproduciblyForTheSameSeed) {
	const auto collect = [](uint64_t master) {
		std::vector<std::string> log;
		engine::Queue            queue(master);
		for (int index = 0; index < 8; index++) {
			const std::string label = "task" + std::to_string(index);
			queue.schedule(0, engine::Priority::CHILD_INTAKE, label,
						   [&log, label](engine::TaskContext&) { log.push_back(label); });
		}
		queue.run_tick(0);
		return log;
	};

	const std::vector<std::string> first  = collect(11);
	const std::vector<std::string> second = collect(11);
	EXPECT_EQ(first, second);
	EXPECT_EQ(first.size(), 8u);

	// Equal-key order is deliberately not insertion order.
	std::vector<std::string> sorted = first;
	std::sort(sorted.begin(), sorted.end());
	EXPECT_NE(first, sorted);
	EXPECT_NE(first, collect(12));
}

TEST(TaskQueueTest, ShuffleDoesNotDependOnInsertionOrder) {
	const auto collect = [](bool reversed) {
		std::vector<std::string> log;
		engine::Queue            queue(5);
		std::vector<std::string> labels;
		for (int index = 0; index < 6; index++) {
			labels.push_back("task" + std::to_string(index));
		}
		if (reversed) {
			std::reverse(labels.begin(), labels.end());
		}
		for (const std::string& label : labels) {
			queue.schedule(3, engine::Priority::WORLD_ADVANCE, label,
						   [&log, label](engine::TaskContext&) { log.push_back(label); });
		}
		queue.run_tick(3);
		return log;
	};

	EXPECT_EQ(collect(false), collect(true));
}

TEST(TaskQueueTest, ShuffleVariesFromTickToTick) {
	const auto collect = [](uint64_t tick) {
		std::vector<std::string> log;
		engine::Queue            queue(3);
		for (int index = 0; index < 8; index++) {
			const std::string label = "task" + std::to_string(index);
			queue.schedule(tick, engine::Priority::CHILD_INTAKE, label,
						   [&log, label](engine::TaskContext&) { log.push_back(label); });
		}
		queue.run_tick(tick);
		return log;
	};

	EXPECT_NE(collect(0), collect(1));
}

TEST(TaskQueueTest, RunningATickWithNothingDueIsHarmless) {
	engine::Queue queue(1);
	EXPECT_NO_THROW(queue.run_tick(0));
	EXPECT_TRUE(queue.empty());
}
