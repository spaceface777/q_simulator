#include "task_queue.h"

#include <algorithm>
#include <cassert>

#include "../seed/seed.h"

namespace engine {

	void TaskContext::schedule_same_tick(Priority priority, std::string label, std::function<void(TaskContext&)> run) {
		queue_->schedule_at(key_.due_tick, key_.generation + 1, priority, std::move(label), std::move(run));
	}

	void TaskContext::schedule_future(uint64_t due_tick, Priority priority, std::string label,
									  std::function<void(TaskContext&)> run) {
		queue_->schedule(due_tick, priority, std::move(label), std::move(run));
	}

	void Queue::schedule(uint64_t due_tick, Priority priority, std::string label,
						 std::function<void(TaskContext&)> run) {
		schedule_at(due_tick, 0, priority, std::move(label), std::move(run));
	}

	void Queue::schedule_at(uint64_t due_tick, uint32_t generation, Priority priority, std::string label,
							std::function<void(TaskContext&)> run) {
		const TaskKey key{due_tick, generation, priority};

		auto bucket = buckets_.try_emplace(key).first;
		bucket->second.push_back(Entry{std::move(label), std::move(run)});
	}

	std::vector<Queue::Entry> Queue::take_next_bucket() {
		const auto    bucket = buckets_.begin();
		const TaskKey key    = bucket->first;

		std::vector<Entry> entries = std::move(bucket->second);
		buckets_.erase(bucket);

		const auto by_label = [](const Entry& left, const Entry& right) { return left.label < right.label; };

		// The shuffle needs a canonical order. Child intake is already sorted: the drain loop
		// walks modules then mailbox slots. Every other bucket is filled by handlers that
		// themselves ran shuffled, so their order is not canonical and must be re-sorted.
		if (key.priority == Priority::CHILD_INTAKE) {
			assert(std::is_sorted(entries.begin(), entries.end(), by_label));
		} else {
			std::sort(entries.begin(), entries.end(), by_label);
		}

		seed::shuffle(entries, master_seed_,
					  "shuffle/" + std::to_string(key.due_tick) + "/" + std::to_string(key.generation) + "/" +
						  std::to_string(static_cast<int>(key.priority)));
		return entries;
	}

	void Queue::run_tick(uint64_t tick) {
		while (!buckets_.empty()) {
			const TaskKey next = buckets_.begin()->first;
			if (next.due_tick != tick) {
				// Everything left is due later; the engine never schedules into the past.
				return;
			}

			for (Entry& entry : take_next_bucket()) {
				TaskContext context(this, next);
				entry.run(context);
			}
		}
	}

} // namespace engine
