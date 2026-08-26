#include "frame.h"

#include <cstring>
#include <stdexcept>

namespace rpc {

	namespace {

		// The wire uses little-endian, matching every host this simulation runs on. Fields are encoded
		// explicitly instead of memcpy-ing the struct, to stay independent of its padding.
		void write_u16(uint8_t* out, uint16_t value) {
			out[0] = static_cast<uint8_t>(value & 0xFF);
			out[1] = static_cast<uint8_t>((value >> 8) & 0xFF);
		}

		uint16_t read_u16(const uint8_t* in) {
			return static_cast<uint16_t>(static_cast<uint16_t>(in[0]) | (static_cast<uint16_t>(in[1]) << 8));
		}

		// The buffer compacts once the consumed prefix grows past this, trading memory for fewer moves.
		constexpr std::size_t COMPACT_THRESHOLD = 4096;

	} // namespace

	std::vector<uint8_t> encode(const Frame& frame) {
		if (frame.payload.size() > MAX_PAYLOAD) {
			throw std::length_error("RPC payload of " + std::to_string(frame.payload.size()) + " bytes exceeds " +
									std::to_string(MAX_PAYLOAD));
		}

		std::vector<uint8_t> out(HEADER_SIZE + frame.payload.size());
		out[0] = frame.type;
		out[1] = frame.cmd;
		write_u16(out.data() + 2, frame.call_id);
		write_u16(out.data() + 4, static_cast<uint16_t>(static_cast<int16_t>(frame.payload.size())));
		if (frame.type == MT_RESP || frame.type == MT_ACK) {
			write_u16(out.data() + 6, static_cast<uint16_t>(frame.status));
		} else {
			write_u16(out.data() + 6, frame.flags);
		}

		if (!frame.payload.empty()) {
			std::memcpy(out.data() + HEADER_SIZE, frame.payload.data(), frame.payload.size());
		}

		return out;
	}

	void FrameReader::append(const uint8_t* data, std::size_t length) {
		if (data == nullptr || length == 0) {
			return;
		}

		compact();
		buffer_.insert(buffer_.end(), data, data + length);
	}

	DecodeStatus FrameReader::next(Frame& out) {
		const std::size_t available = buffered();
		if (available < HEADER_SIZE) {
			return DecodeStatus::INCOMPLETE;
		}

		const uint8_t* header = buffer_.data() + consumed_;

		// payload_len is signed on the wire. A negative value means the stream is unrecoverable.
		const int16_t payload_len = static_cast<int16_t>(read_u16(header + 4));
		if (payload_len < 0 || static_cast<std::size_t>(payload_len) > MAX_PAYLOAD) {
			return DecodeStatus::INVALID;
		}

		const std::size_t frame_size = HEADER_SIZE + static_cast<std::size_t>(payload_len);
		if (available < frame_size) {
			return DecodeStatus::INCOMPLETE;
		}

		out.type    = header[0];
		out.cmd     = header[1];
		out.call_id = read_u16(header + 2);
		out.flags   = read_u16(header + 6);
		out.status  = static_cast<int16_t>(out.flags);
		out.payload.assign(header + HEADER_SIZE, header + frame_size);
		out.observation_request_id = 0;
		out.handler_work_id        = 0;

		consumed_ += frame_size;
		return DecodeStatus::OK;
	}

	void FrameReader::compact() {
		if (consumed_ == 0) {
			return;
		}

		if (consumed_ == buffer_.size()) {
			buffer_.clear();
			consumed_ = 0;
			return;
		}

		if (consumed_ < COMPACT_THRESHOLD) {
			return;
		}

		buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(consumed_));
		consumed_ = 0;
	}

	void FrameReader::clear() noexcept {
		buffer_.clear();
		consumed_ = 0;
	}

} // namespace rpc
