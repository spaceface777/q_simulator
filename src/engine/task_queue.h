#ifndef ENGINE_TASK_QUEUE_H
#define ENGINE_TASK_QUEUE_H

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace engine {

	// Tasks run in order of (due tick, generation, priority). Within one bucket the order is a
	// seeded shuffle, so a run cannot silently depend on insertion order.
	enum class Priority : int {
		CHILD_INTAKE      = 0, // What the applications sent.
		SCENARIO_EVENTS   = 1, // Stimuli from events.toml.
		WORLD_ADVANCE     = 2, // Physical state.
		PLATFORM_OUTBOUND = 3, // What the applications are told.
		MONITORING        = 4, // Temporal properties over the resulting state.
		TRACE_FINALIZE    = 5, // The record of what happened.
	};

	struct TaskKey {
		uint64_t due_tick   = 0;
		uint32_t generation = 0;
		Priority priority   = Priority::CHILD_INTAKE;

		bool operator<(const TaskKey& other) const noexcept {
			if (due_tick != other.due_tick) {
				return due_tick < other.due_tick;
			}
			if (generation != other.generation) {
				return generation < other.generation;
			}
			return priority < other.priority;
		}
	};

	class Queue;

	// Lets a running task schedule further work without reaching for the queue directly.
	class TaskContext {
	private:
		Queue*  queue_;
		TaskKey key_;

	public:
		TaskContext(Queue* queue, TaskKey key) : queue_(queue), key_(key) {}

		[[nodiscard]] uint64_t tick() const noexcept { return key_.due_tick; }

		[[nodiscard]] uint32_t generation() const noexcept { return key_.generation; }

		// Work scheduled this way never jumps ahead of work already due in the current tick.
		void schedule_same_tick(Priority priority, std::string label, std::function<void(TaskContext&)> run);

		void schedule_future(uint64_t due_tick, Priority priority, std::string label,
							 std::function<void(TaskContext&)> run);
	};

	class Queue {
	private:
		struct Entry {
			std::string                       label;
			std::function<void(TaskContext&)> run;
		};

		uint64_t                              master_seed_;
		std::map<TaskKey, std::vector<Entry>> buckets_;

		[[nodiscard]] std::vector<Entry> take_next_bucket();

	public:
		explicit Queue(uint64_t master_seed) : master_seed_(master_seed) {}

		// The label must be unique within its bucket; it drives the shuffle order.
		void schedule(uint64_t due_tick, Priority priority, std::string label, std::function<void(TaskContext&)> run);

		void schedule_at(uint64_t due_tick, uint32_t generation, Priority priority, std::string label,
						 std::function<void(TaskContext&)> run);

		// Runs every task due at the tick, including work those tasks create for the same tick.
		void run_tick(uint64_t tick);

		[[nodiscard]] bool empty() const noexcept { return buckets_.empty(); }

		[[nodiscard]] std::size_t size() const noexcept { return buckets_.size(); }

		[[nodiscard]] std::optional<uint64_t> next_due_tick() const noexcept {
			if (buckets_.empty()) return std::nullopt;
			return buckets_.begin()->first.due_tick;
		}
	};

} // namespace engine

#endif // ENGINE_TASK_QUEUE_H
