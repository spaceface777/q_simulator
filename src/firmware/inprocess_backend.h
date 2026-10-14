#pragma once
#include "backend.h"
#include "image.h"
#include <array>
#include <map>
#include <memory>
#include <quercus/sim_instance.h>

namespace firmware {
	class InProcessBackend {
	public:
		InProcessBackend(std::span<const ImageBinding> bindings, uint64_t seed,
						 const std::map<std::string, std::string>& environment = {});
		~InProcessBackend();
		InProcessBackend(const InProcessBackend&)            = delete;
		InProcessBackend& operator=(const InProcessBackend&) = delete;
		void              execute_epoch(std::span<ExecutionGrant> grants);
		void              retire(module::ModuleId module);
		void              stop() noexcept;

	private:
		struct Instance {
			LoadedImage             image;
			const qsim_image_api_t* api;
			uint64_t                time = 0, next_wake = 0;
			bool                    bound = false, has_run = false;

			explicit Instance(const std::string& path)
				: image(path), api(static_cast<const qsim_image_api_t*>(image.api())) {}
		};

		std::array<std::unique_ptr<Instance>, MODULE_TABLE_SIZE> instances_;
	};
} // namespace firmware
