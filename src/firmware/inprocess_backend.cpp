#include "inprocess_backend.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace firmware {
	namespace {
		std::runtime_error failure(module::ModuleId module, const char* operation, const std::string& detail) {
			return std::runtime_error("Module " + std::to_string(module.raw()) + " firmware " + operation + ": " +
									  detail);
		}

		void check(module::ModuleId module, const char* operation, qsim_status_t status) {
			if (status != QSIM_STATUS_OK) throw failure(module, operation, "status " + std::to_string(status));
		}
	} // namespace

	InProcessBackend::InProcessBackend(std::span<const ImageBinding> bindings, uint64_t seed,
									   const std::map<std::string, std::string>& environment) {
		std::vector<std::string> entries;
		entries.reserve(environment.size());
		for (const auto& [key, value] : environment) {
			if (key.empty() || key.find('=') != std::string::npos || key.find('\0') != std::string::npos ||
				value.find('\0') != std::string::npos)
				throw std::invalid_argument("Invalid firmware environment entry");
			entries.push_back(key + "=" + value);
		}
		std::vector<const char*> pointers;
		for (const auto& entry : entries) pointers.push_back(entry.c_str());
		try {
			for (const auto& binding : bindings) {
				auto& instance = instances_[binding.module.raw()];
				if (instance) throw failure(binding.module, "bind", "duplicate module");
				try {
					instance = std::make_unique<Instance>(binding.path);
				} catch (const std::exception& error) {
					throw failure(binding.module, "load", error.what());
				}
				const qsim_image_config_t config{binding.module.raw(), seed, pointers.data(), pointers.size()};
				check(binding.module, "bind", instance->api->bind(&config));
				instance->bound = true;
			}
		} catch (...) {
			stop();
			throw;
		}
	}

	InProcessBackend::~InProcessBackend() {
		stop();
	}

	void InProcessBackend::execute_epoch(std::span<ExecutionGrant> grants) {
		for (auto& grant : grants) {
			grant.outbound_used = 0;
			grant.outbound      = FrameBatchView::from_bytes(grant.outbound_storage, 0);
			if (!grant.active || !grant.granted) continue;
			auto& image = instances_[grant.module.raw()];
			if (!image || !image->bound) throw failure(grant.module, "run", "image is not active");
			if (!grant.delta_us || image->time > UINT64_MAX - grant.delta_us)
				throw failure(grant.module, "run", "invalid native-time window");
			const uint64_t start = image->time;
			image->time += grant.delta_us;
			if (image->has_run && !grant.inbound_count && image->next_wake > image->time) continue;
			const auto input = FrameBatchView::from_bytes(grant.inbound, grant.inbound_used);
			if (!input.valid() || input.size() != grant.inbound_count)
				throw failure(grant.module, "run", "invalid input batch");
			std::array<qsim_frame_t, QSIM_MAX_GRANT_FRAMES> frames{};
			for (std::size_t i = 0; i < input.size(); ++i)
				frames[i] = {input[i].data(), static_cast<uint32_t>(input[i].size())};
			const qsim_grant_t  request{start, image->time, frames.data(), static_cast<uint32_t>(input.size())};
			qsim_grant_result_t result{};
			const auto          status = image->api->run(&request, &result);
			if (status != QSIM_STATUS_OK || result.outcome != QSIM_OUTCOME_GRANT_COMPLETE)
				throw failure(grant.module, "run",
							  "status " + std::to_string(status) + ", outcome " + std::to_string(result.outcome) +
								  ": " + (result.diagnostic ? result.diagnostic : "no diagnostic"));
			const auto output = FrameBatchView::from_frames(result.outbound_frames, result.outbound_count);
			if (!output.valid()) throw failure(grant.module, "run", "invalid output batch");
			uint32_t count = 0;
			for (std::size_t i = 0; i < output.size(); ++i) {
				const auto frame = output[i];
				if (qsim_frame_batch_append(&grant.outbound_storage, &grant.outbound_used, count++, frame.data(),
											static_cast<uint32_t>(frame.size())) != QSIM_BATCH_OK)
					throw failure(grant.module, "run", "output overflow");
			}
			grant.outbound = FrameBatchView::from_bytes(grant.outbound_storage, grant.outbound_used);
			if (!grant.outbound.valid()) throw failure(grant.module, "run", "invalid output batch");
			image->next_wake = result.next_wake_us;
			image->has_run   = true;
		}
	}

	void InProcessBackend::retire(module::ModuleId module) {
		auto& image = instances_[module.raw()];
		if (!image) return;
		if (image->bound) {
			image->bound = false;
			check(module, "destroy", image->api->destroy());
		}
		image.reset();
	}

	void InProcessBackend::stop() noexcept {
		for (std::size_t id = 1; id < instances_.size(); ++id) {
			if (!instances_[id]) continue;
			try {
				retire(module::ModuleId{static_cast<uint8_t>(id)});
			} catch (const std::exception& error) {
				std::cerr << error.what() << '\n';
				instances_[id].reset();
			}
		}
	}
} // namespace firmware
