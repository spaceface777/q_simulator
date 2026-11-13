#include "inprocess_backend.h"

#include <stdexcept>
#include <string>

namespace firmware {
	namespace {
		std::runtime_error failure(module::ModuleId module, const std::string& detail) {
			return std::runtime_error("Module " + std::to_string(module.raw()) + " firmware: " + detail);
		}
	} // namespace

	InProcessBackend::InProcessBackend(std::span<const ImageBinding> bindings, uint64_t seed) {
		for (const auto& [module, api] : bindings) {
			auto& instance = instances_[module.raw()];
			if (instance.api || !api || api->abi_version != QSIM_IMAGE_ABI_VERSION || !api->bind || !api->run)
				throw failure(module, "invalid image binding");
			if (const int status = api->bind(static_cast<uint32_t>(seed)); status != 0)
				throw failure(module, "bind returned " + std::to_string(status));
			instance.api = api;
		}
	}

	void InProcessBackend::execute_epoch(std::span<ExecutionGrant> grants) {
		for (auto& grant : grants) {
			quercus_mailbox_clear(&grant.outbound);
			if (!grant.granted) continue;
			auto& image = instances_[grant.module.raw()];
			if (!image.api || !grant.delta_us || image.time >= UINT64_MAX - grant.delta_us)
				throw failure(grant.module, "invalid grant");
			const uint64_t start = image.time;
			image.time += grant.delta_us;
			if (image.next_wake > image.time && quercus_mailbox_count(&grant.inbound) == 0) continue;
			const int status = image.api->run(start, image.time, &grant.inbound, &grant.outbound, &image.next_wake);
			if (status != 0) throw failure(grant.module, "run returned " + std::to_string(status));
			if (!quercus_mailbox_valid(&grant.outbound)) throw failure(grant.module, "invalid output batch");
		}
	}

	void InProcessBackend::retire(module::ModuleId module) {
		instances_[module.raw()].api = nullptr;
	}
} // namespace firmware
