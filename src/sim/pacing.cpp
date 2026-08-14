#include "pacing.h"

#include <atomic>

namespace sim {

	namespace {

		// Atomics instead of a lock: a torn read only mislabels one frame.
		std::atomic<double> configured{1.0};
		std::atomic<double> effective{1.0};
		std::atomic<bool>   behind{false};

	} // namespace

	void publish_pacing(const Pacing& pacing) {
		configured.store(pacing.configured, std::memory_order_relaxed);
		effective.store(pacing.effective, std::memory_order_relaxed);
		behind.store(pacing.behind, std::memory_order_relaxed);
	}

	Pacing pacing() {
		Pacing current;
		current.configured = configured.load(std::memory_order_relaxed);
		current.effective  = effective.load(std::memory_order_relaxed);
		current.behind     = behind.load(std::memory_order_relaxed);
		return current;
	}

} // namespace sim
