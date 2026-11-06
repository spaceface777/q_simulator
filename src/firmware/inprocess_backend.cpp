#include "inprocess_backend.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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

		// Calls into an image go through crash::run_guarded, which takes a plain function. These
		// carry the arguments. They hold no objects with destructors, because a fault abandons the
		// call without unwinding it.
		struct RunCall {
			const qsim_image_api_t* api;
			const qsim_grant_t*     request;
			qsim_grant_result_t*    result;
			qsim_status_t           status = QSIM_STATUS_IO_ERROR;

			static void invoke(void* self) {
				auto& call  = *static_cast<RunCall*>(self);
				call.status = call.api->run(call.request, call.result);
			}
		};

		struct DestroyCall {
			const qsim_image_api_t* api;
			qsim_status_t           status = QSIM_STATUS_IO_ERROR;

			static void invoke(void* self) {
				auto& call  = *static_cast<DestroyCall*>(self);
				call.status = call.api->destroy();
			}
		};

		struct ThreadCall {
			const qsim_image_api_t* api;
			char                    name[96] = {};

			static void invoke(void* self) {
				auto&       call  = *static_cast<ThreadCall*>(self);
				const char* found = call.api->current_thread();
				if (found) std::strncpy(call.name, found, sizeof(call.name) - 1);
			}
		};

		struct LogCall {
			const qsim_image_api_t* api;
			char*                   buffer;
			std::size_t             capacity;
			std::size_t             length = 0;

			static void invoke(void* self) {
				auto& call  = *static_cast<LogCall*>(self);
				call.length = call.api->recent_log(call.buffer, call.capacity);
			}
		};
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
				const qsim_image_config_t config{
					binding.module.raw(),        seed,          pointers.data(), pointers.size(),
					&InProcessBackend::on_fatal, instance.get()};
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

	// Runs on the application's stack, before the image unwinds, so the stack says where it ended.
	void InProcessBackend::on_fatal(void* context, const char* description) {
		auto& instance = *static_cast<Instance*>(context);
		crash::capture_here(instance.ended);
		instance.ended.description.fill('\0');
		if (description)
			std::strncpy(instance.ended.description.data(), description, instance.ended.description.size() - 1);
		instance.ended_recorded = true;
	}

	std::vector<crash::ImageName> InProcessBackend::image_names() const {
		std::vector<crash::ImageName> names;
		for (const auto& instance : instances_)
			if (instance)
				names.push_back({instance->image.loaded_path().string(),
								 std::filesystem::path(instance->source).filename().string()});
		return names;
	}

	ModuleFailure InProcessBackend::stop_application(const ExecutionGrant& grant, Instance& image,
													 const crash::Capture& capture) {
		image.failed = true;

		crash::ReportContext context;
		context.module         = grant.module.raw();
		context.image          = image.source;
		context.global_tick    = grant.global_tick;
		context.module_time_us = image.time;
		context.images         = image_names();

		// The application's own record of what it was doing. Either call may fault in an image
		// that has lost its integrity, so both are guarded too.
		crash::Capture ignored;
		if (image.api->current_thread) {
			ThreadCall call{image.api};
			if (crash::run_guarded(&ThreadCall::invoke, &call, ignored)) context.thread = call.name;
			else image.abandoned = true;
		}
		if (image.api->recent_log) {
			std::vector<char> buffer(4096);
			LogCall           call{image.api, buffer.data(), buffer.size()};
			if (crash::run_guarded(&LogCall::invoke, &call, ignored))
				context.recent_log.assign(buffer.data(), std::min(call.length, buffer.size()));
			else image.abandoned = true;
		}
		if (image.abandoned) {
			image.image.abandon();
			image.bound = false;
		}
		return ModuleFailure{crash::summary(capture), crash::report(capture, context)};
	}

	void InProcessBackend::execute_epoch(std::span<ExecutionGrant> grants) {
		for (auto& grant : grants) {
			grant.outbound_used = 0;
			grant.outbound      = FrameBatchView::from_bytes(grant.outbound_storage, 0);
			grant.failure.reset();
			if (!grant.active || !grant.granted) continue;
			auto& image = instances_[grant.module.raw()];
			if (!image || !image->bound || image->failed) throw failure(grant.module, "run", "image is not active");
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

			RunCall        call{image->api, &request, &result};
			crash::Capture capture;
			image->ended_recorded = false;
			if (!crash::run_guarded(&RunCall::invoke, &call, capture)) {
				image->abandoned = true;
				grant.failure    = stop_application(grant, *image, capture);
				continue;
			}
			if (call.status == QSIM_STATUS_OK && result.outcome == QSIM_OUTCOME_NATIVE_FATAL) {
				crash::Capture ended = image->ended_recorded ? image->ended : crash::Capture{};
				ended.cause          = crash::Cause::ENDED;
				if (ended.description[0] == '\0')
					std::strncpy(ended.description.data(), result.diagnostic ? result.diagnostic : "fatal outcome",
								 ended.description.size() - 1);
				grant.failure = stop_application(grant, *image, ended);
				continue;
			}
			if (call.status != QSIM_STATUS_OK || result.outcome != QSIM_OUTCOME_GRANT_COMPLETE)
				throw failure(grant.module, "run",
							  "status " + std::to_string(call.status) + ", outcome " + std::to_string(result.outcome) +
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
			DestroyCall    call{image->api};
			crash::Capture capture;
			if (!crash::run_guarded(&DestroyCall::invoke, &call, capture)) {
				image->image.abandon();
				std::cerr << "Module " << static_cast<unsigned>(module.raw())
						  << " firmware destroy failed: " << crash::summary(capture)
						  << ". Its library stays loaded until the simulator exits." << std::endl;
			} else if (!image->failed) {
				check(module, "destroy", call.status);
			}
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
