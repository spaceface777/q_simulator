#ifndef PLATFORM_RPC_WORKER_POOL_H
#define PLATFORM_RPC_WORKER_POOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

#include "platform.h"

namespace platform {

	// Core0 has three equal-priority RPC workers. An idle worker takes the oldest queued request
	// whose category no other worker is handling, so a busy category never blocks a worker.
	inline constexpr std::size_t RPC_HANDLER_LANES = 3;

	class RpcWorkerPool {
	public:
		using RequestId = uint64_t;

	private:
		struct Work {
			RequestId id;
			Bucket    bucket;
		};

		std::array<std::optional<Work>, RPC_HANDLER_LANES> lanes_;
		std::deque<Work>                                   queued_;

		[[nodiscard]] bool bucket_busy(Bucket bucket) const;

		void dispatch(std::vector<RequestId>& started);

	public:
		// Returns the request id when the request starts immediately. Otherwise it waits in the
		// queue until a worker is idle and no other worker handles its category.
		[[nodiscard]] std::optional<RequestId> enqueue(RequestId id, Bucket bucket);

		// Releases a completed request, then lets idle workers take eligible queued requests.
		// Returned request ids started in deterministic lane order.
		[[nodiscard]] std::vector<RequestId> complete(RequestId id);

		[[nodiscard]] bool is_running(RequestId id) const;

		[[nodiscard]] std::size_t occupied() const;

		[[nodiscard]] std::size_t queued() const noexcept { return queued_.size(); }
	};

} // namespace platform

#endif // PLATFORM_RPC_WORKER_POOL_H
