#pragma once
#include "backend.h"
#include "crash.h"
#include "image.h"
#include <array>
#include <map>
#include <memory>
#include <quercus/sim_instance.h>
#include <vector>

namespace firmware {
	class InProcessBackend {
	public:
		InProcessBackend(std::span<const ImageBinding> bindings, uint64_t seed,
						 const std::map<std::string, std::string>& environment = {});
		~InProcessBackend();
		InProcessBackend(const InProcessBackend&)            = delete;
		InProcessBackend& operator=(const InProcessBackend&) = delete;
		// Runs each granted module. An application that crashes, hangs, or ends itself does not
		// stop the epoch: its grant reports the failure, and that module is never run again.
		void execute_epoch(std::span<ExecutionGrant> grants);
		void retire(module::ModuleId module);
		void stop() noexcept;

	private:
		struct Instance {
			LoadedImage             image;
			const qsim_image_api_t* api;
			std::string             source;
			uint64_t                time = 0, next_wake = 0;
			bool                    bound = false, has_run = false;
			// The application stopped. Only the crash diagnostics may still be called.
			bool failed = false;
			// It faulted or hung mid-execution. No call may enter it again, not even destroy.
			bool abandoned = false;
			// Where the application ended itself, recorded by its fatal hook.
			crash::Capture ended{};
			bool           ended_recorded = false;

			explicit Instance(const std::string& path)
				: image(path), api(static_cast<const qsim_image_api_t*>(image.api())), source(path) {}
		};

		static void on_fatal(void* context, const char* description);

		ModuleFailure stop_application(const ExecutionGrant& grant, Instance& image, const crash::Capture& capture);
		std::vector<crash::ImageName> image_names() const;

		std::array<std::unique_ptr<Instance>, MODULE_TABLE_SIZE> instances_;
	};
} // namespace firmware
