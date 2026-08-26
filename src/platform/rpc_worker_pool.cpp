#include "rpc_worker_pool.h"

#include <algorithm>

namespace platform {

	bool RpcWorkerPool::bucket_running(Bucket bucket) const {
		return std::ranges::any_of(lanes_, [bucket](const Lane& lane) {
			return lane.running && lane.work.has_value() && lane.work->bucket == bucket;
		});
	}

	void RpcWorkerPool::start_waiting(std::vector<RequestId>& started) {
		for (Lane& lane : lanes_) {
			if (!lane.work.has_value() || lane.running || bucket_running(lane.work->bucket)) {
				continue;
			}
			lane.running = true;
			started.push_back(lane.work->id);
		}
	}

	void RpcWorkerPool::fill_free_lanes(std::vector<RequestId>& started) {
		for (Lane& lane : lanes_) {
			if (lane.work.has_value() || queued_.empty()) {
				continue;
			}
			lane.work = queued_.front();
			queued_.pop_front();
		}
		start_waiting(started);
	}

	std::optional<RpcWorkerPool::RequestId> RpcWorkerPool::enqueue(RequestId id, Bucket bucket) {
		std::vector<RequestId> started;
		queued_.push_back(Work{id, bucket});
		fill_free_lanes(started);
		return started.empty() ? std::nullopt : std::optional<RequestId>{started.front()};
	}

	std::vector<RpcWorkerPool::RequestId> RpcWorkerPool::complete(RequestId id) {
		for (Lane& lane : lanes_) {
			if (lane.running && lane.work.has_value() && lane.work->id == id) {
				lane = Lane{};
				break;
			}
		}

		std::vector<RequestId> started;
		// Existing lane owners reached the category mutex before requests still outside the pool.
		start_waiting(started);
		fill_free_lanes(started);
		return started;
	}

	bool RpcWorkerPool::is_occupied(RequestId id) const {
		return std::ranges::any_of(lanes_,
								   [id](const Lane& lane) { return lane.work.has_value() && lane.work->id == id; });
	}

	bool RpcWorkerPool::is_running(RequestId id) const {
		return std::ranges::any_of(
			lanes_, [id](const Lane& lane) { return lane.running && lane.work.has_value() && lane.work->id == id; });
	}

	std::size_t RpcWorkerPool::occupied() const {
		return static_cast<std::size_t>(
			std::ranges::count_if(lanes_, [](const Lane& lane) { return lane.work.has_value(); }));
	}

} // namespace platform
