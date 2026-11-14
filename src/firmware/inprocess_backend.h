#pragma once

#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <quercus/sim_instance.h>

#include "../module/types.h"
#include "crash.h"

namespace firmware {

	struct ImageBinding {
		module::ModuleId                       module;
		const qsim_image_api_t*                api;
		std::optional<std::array<uint8_t, 32>> digest{};
	};

	struct ExecutionGrant {
		module::ModuleId              module;
		bool                          granted  = false;
		uint32_t                      delta_us = 0;
		quercus_mailbox_t             inbound{};
		quercus_mailbox_t             outbound{};
		std::optional<crash::Capture> crash{};
		uint64_t                      module_time_us = 0;
	};

	class InProcessBackend {
	public:
		InProcessBackend(std::span<const ImageBinding> bindings, uint64_t seed,
						 const std::map<std::string, std::string>& environment);
		void execute_epoch(std::span<ExecutionGrant> grants);
		void retire(module::ModuleId module);

	private:
		struct Instance {
			const qsim_image_api_t* api       = nullptr;
			uint64_t                time      = 0;
			uint64_t                next_wake = 0;
		};

		std::vector<qsim_env_entry_t>                   environment_;
		std::array<Instance, QUERCUS_MODULE_TABLE_SIZE> instances_{};
	};
} // namespace firmware
