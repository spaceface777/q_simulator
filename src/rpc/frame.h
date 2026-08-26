#ifndef RPC_FRAME_H
#define RPC_FRAME_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include <quercus/flight_board.h>
#include <quercus/rpc_wire.h>

namespace rpc {

	inline constexpr std::size_t HEADER_SIZE = sizeof(rpc_message_t);

	inline constexpr std::size_t MAX_PAYLOAD = static_cast<std::size_t>(RPC_MAX_PAYLOAD);

	// The final two header bytes are a union on the wire: flags for requests and events, a status
	// for responses. Both fields decode from the same bytes; encode writes only the matching one.
	struct Frame {
		uint8_t              type    = 0;
		uint8_t              cmd     = 0;
		uint16_t             call_id = 0;
		uint16_t             flags   = 0;
		int16_t              status  = 0;
		std::vector<uint8_t> payload;

		// Simulator-only correlation metadata. It is deliberately absent from encode/decode and
		// follows a response through the platform backlog to its actual delivery boundary.
		uint64_t observation_request_id = 0;

		// Simulator-only Core0 worker ownership. A confirmable request keeps its worker lane and
		// category mutex until this response is handed to the requesting module.
		uint64_t handler_work_id = 0;
	};

	enum class DecodeStatus {
		OK,         // A complete frame was decoded and removed from the buffer.
		INCOMPLETE, // Not enough bytes buffered yet; call again after more have arrived.
		INVALID,    // The buffered bytes are not a valid frame and the run must stop.
	};

	// Serializes a frame, header first. Throws std::length_error if the payload exceeds MAX_PAYLOAD.
	[[nodiscard]] std::vector<uint8_t> encode(const Frame& frame);

	// Reassembles whole frames from received byte chunks.
	class FrameReader {
	private:
		std::vector<uint8_t> buffer_;
		std::size_t          consumed_ = 0;

		void compact();

	public:
		void append(const uint8_t* data, std::size_t length);

		// out is populated only when the result is OK.
		[[nodiscard]] DecodeStatus next(Frame& out);

		[[nodiscard]] std::size_t buffered() const noexcept { return buffer_.size() - consumed_; }

		void clear() noexcept;
	};

} // namespace rpc

#endif // RPC_FRAME_H
