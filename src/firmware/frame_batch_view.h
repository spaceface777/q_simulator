#ifndef SIM_FRAME_BATCH_VIEW_H
#define SIM_FRAME_BATCH_VIEW_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <quercus/rpc_wire.h>
#include <quercus/sim_instance.h>
#include <quercus/sim_transport.h>

namespace firmware {

	// A non-owning, transport-neutral view of one bounded firmware handoff. The
	// factories validate RPC metadata before any frame bytes are exposed.
	class FrameBatchView {
	public:
		using Frame = std::span<const uint8_t>;

		// Recover frame boundaries from the RPC headers in an opaque byte batch.
		[[nodiscard]] static FrameBatchView from_bytes(const qsim_frame_batch_t& batch, uint32_t used) noexcept {
			FrameBatchView view;
			if (used > QSIM_MAX_BATCH_BYTES) return view;

			uint32_t offset = 0;
			while (offset < used) {
				if (view.count_ == QSIM_MAX_GRANT_FRAMES || used - offset < sizeof(rpc_message_t))
					return FrameBatchView{};
				const uint8_t* header = batch.data + offset;
				const uint16_t encoded_length =
					static_cast<uint16_t>(header[4]) | (static_cast<uint16_t>(header[5]) << 8U);
				const int16_t payload_length = static_cast<int16_t>(encoded_length);
				if (payload_length < 0 || payload_length > RPC_MAX_PAYLOAD) return FrameBatchView{};
				const uint32_t frame_size = sizeof(rpc_message_t) + static_cast<uint32_t>(payload_length);
				if (frame_size > used - offset) return FrameBatchView{};
				view.frames_[view.count_++] = Frame(header, frame_size);
				offset += frame_size;
			}

			view.valid_ = true;
			return view;
		}

		[[nodiscard]] static FrameBatchView from_frames(const qsim_frame_t* frames, uint32_t count) noexcept {
			FrameBatchView view;
			if (count > QSIM_MAX_GRANT_FRAMES || (count != 0 && frames == nullptr)) return view;

			uint32_t total = 0;
			for (uint32_t index = 0; index < count; index++) {
				const qsim_frame_t& frame = frames[index];
				if (frame.data == nullptr || frame.size == 0 || frame.size > QSIM_MAX_BATCH_BYTES - total)
					return FrameBatchView{};
				total += frame.size;
				view.frames_[index] = Frame(frame.data, frame.size);
			}

			view.count_ = count;
			view.valid_ = true;
			return view;
		}

		[[nodiscard]] bool valid() const noexcept { return valid_; }

		[[nodiscard]] std::size_t size() const noexcept { return count_; }

		[[nodiscard]] Frame operator[](std::size_t index) const noexcept { return frames_[index]; }

	private:
		std::array<Frame, QSIM_MAX_GRANT_FRAMES> frames_{};
		std::size_t                              count_ = 0;
		bool                                     valid_ = false;
	};

} // namespace firmware

#endif // SIM_FRAME_BATCH_VIEW_H
