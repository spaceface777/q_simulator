#include "rpc_worker_pool.h"

#include <algorithm>

namespace platform {

	bool RpcWorkerPool::bucket_busy(Bucket bucket) const {
		// Unknown commands have no category, so any number of them can run together.
		if (bucket == Bucket::GLOBAL) return false;
		return std::ranges::any_of(
			lanes_, [bucket](const std::optional<Work>& lane) { return lane && lane->bucket == bucket; });
	}

	void RpcWorkerPool::dispatch(std::vector<RequestId>& started) {
		for (std::optional<Work>& lane : lanes_) {
			if (lane.has_value()) continue;
			// Skipped requests keep their queue order.
			const auto eligible =
				std::ranges::find_if(queued_, [this](const Work& work) { return !bucket_busy(work.bucket); });
			if (eligible == queued_.end()) return;
			lane = *eligible;
			queued_.erase(eligible);
			started.push_back(lane->id);
		}
	}

	std::optional<RpcWorkerPool::RequestId> RpcWorkerPool::enqueue(RequestId id, Bucket bucket) {
		std::vector<RequestId> started;
		queued_.push_back(Work{id, bucket});
		dispatch(started);
		return started.empty() ? std::nullopt : std::optional<RequestId>{started.front()};
	}

	std::vector<RpcWorkerPool::RequestId> RpcWorkerPool::complete(RequestId id) {
		for (std::optional<Work>& lane : lanes_) {
			if (lane && lane->id == id) {
				lane.reset();
				break;
			}
		}

		std::vector<RequestId> started;
		dispatch(started);
		return started;
	}

	bool RpcWorkerPool::is_running(RequestId id) const {
		return std::ranges::any_of(lanes_, [id](const std::optional<Work>& lane) { return lane && lane->id == id; });
	}

	std::size_t RpcWorkerPool::occupied() const {
		return static_cast<std::size_t>(
			std::ranges::count_if(lanes_, [](const std::optional<Work>& lane) { return lane.has_value(); }));
	}

} // namespace platform
