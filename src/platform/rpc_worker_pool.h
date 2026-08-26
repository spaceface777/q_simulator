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

	// Core0 has three equal-priority RPC workers. A request owns its lane even while it waits for
	// another request in the same category to release that category's mutex.
	inline constexpr std::size_t RPC_HANDLER_LANES = 3;

	class RpcWorkerPool {
	public:
		using RequestId = uint64_t;

	private:
		struct Work {
			RequestId id;
			Bucket    bucket;
		};

		struct Lane {
			std::optional<Work> work;
			bool                running = false;
		};

		std::array<Lane, RPC_HANDLER_LANES> lanes_;
		std::deque<Work>                    queued_;

		[[nodiscard]] bool bucket_running(Bucket bucket) const;

		void start_waiting(std::vector<RequestId>& started);

		void fill_free_lanes(std::vector<RequestId>& started);

	public:
		// Returns the request id when the request starts immediately. A request can instead occupy a
		// lane waiting for its category mutex, or wait outside the pool when all lanes are occupied.
		[[nodiscard]] std::optional<RequestId> enqueue(RequestId id, Bucket bucket);

		// Releases a completed request, starts eligible lane waiters, then admits queued requests to
		// the free lane. Returned request ids started in deterministic lane order.
		[[nodiscard]] std::vector<RequestId> complete(RequestId id);

		[[nodiscard]] bool is_occupied(RequestId id) const;

		[[nodiscard]] bool is_running(RequestId id) const;

		[[nodiscard]] std::size_t occupied() const;

		[[nodiscard]] std::size_t queued() const noexcept { return queued_.size(); }
	};

} // namespace platform

#endif // PLATFORM_RPC_WORKER_POOL_H
