#ifndef FIRMWARE_BACKEND_H
#define FIRMWARE_BACKEND_H

#include <cstdint>
#include <span>
#include <string>

#include <quercus/sim_transport.h>

#include "../module/types.h"
#include "frame_batch_view.h"

namespace firmware {

	struct ImageBinding {
		module::ModuleId module;
		std::string      path;
	};

	// One module's prepared work and buffered result for a strict execution epoch.
	// The backend exposes output only after every participating module has run.
	struct ExecutionGrant {
		module::ModuleId   module;
		bool               active      = false;
		bool               granted     = false;
		uint32_t           delta_us    = 0;
		uint64_t           global_tick = 0;
		uint64_t           local_tick  = 0;
		qsim_frame_batch_t inbound{};
		uint32_t           inbound_used  = 0;
		uint32_t           inbound_count = 0; // Writer-owned; not part of the transport.
		// Result storage remains stable until the next epoch.
		qsim_frame_batch_t outbound_storage{};
		uint32_t           outbound_used = 0;
		FrameBatchView     outbound;

		// Starts another epoch without clearing the bounded byte buffers. The used-byte
		// counts and view are the authority, so bytes left by an earlier epoch stay hidden.
		void reset_metadata(module::ModuleId next_module, bool next_active) noexcept {
			module        = next_module;
			active        = next_active;
			granted       = false;
			delta_us      = 0;
			global_tick   = 0;
			local_tick    = 0;
			inbound_used  = 0;
			inbound_count = 0;
			outbound_used = 0;
			outbound      = {};
		}
	};

} // namespace firmware

#endif // FIRMWARE_BACKEND_H
