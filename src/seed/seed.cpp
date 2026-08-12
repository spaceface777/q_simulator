#include "seed.h"

#include <algorithm>
#include <cstring>

namespace seed {

	namespace {

		// Versioned: changing it changes every random choice, and it matches the Go implementation.
		constexpr char        NAMESPACE_PREFIX[]      = "quercus-sim/v1\0";
		constexpr std::size_t NAMESPACE_PREFIX_LENGTH = sizeof(NAMESPACE_PREFIX) - 1;

		// FIPS 180-4, inlined to match the Go derivation without a cryptography dependency.

		constexpr uint32_t ROUND_CONSTANTS[64] = {
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
			0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
			0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
			0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
			0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
			0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
			0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
			0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

		uint32_t rotate_right(uint32_t value, unsigned bits) {
			return (value >> bits) | (value << (32 - bits));
		}

		void compress(uint32_t state[8], const uint8_t block[64]) {
			uint32_t schedule[64];
			for (std::size_t index = 0; index < 16; index++) {
				schedule[index] = (static_cast<uint32_t>(block[index * 4]) << 24) |
								  (static_cast<uint32_t>(block[(index * 4) + 1]) << 16) |
								  (static_cast<uint32_t>(block[(index * 4) + 2]) << 8) |
								  static_cast<uint32_t>(block[(index * 4) + 3]);
			}
			for (std::size_t index = 16; index < 64; index++) {
				const uint32_t s0 = rotate_right(schedule[index - 15], 7) ^ rotate_right(schedule[index - 15], 18) ^
									(schedule[index - 15] >> 3);
				const uint32_t s1 = rotate_right(schedule[index - 2], 17) ^ rotate_right(schedule[index - 2], 19) ^
									(schedule[index - 2] >> 10);
				schedule[index]   = schedule[index - 16] + s0 + schedule[index - 7] + s1;
			}

			uint32_t a = state[0];
			uint32_t b = state[1];
			uint32_t c = state[2];
			uint32_t d = state[3];
			uint32_t e = state[4];
			uint32_t f = state[5];
			uint32_t g = state[6];
			uint32_t h = state[7];

			for (std::size_t index = 0; index < 64; index++) {
				const uint32_t s1       = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
				const uint32_t choose   = (e & f) ^ (~e & g);
				const uint32_t temp1    = h + s1 + choose + ROUND_CONSTANTS[index] + schedule[index];
				const uint32_t s0       = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
				const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
				const uint32_t temp2    = s0 + majority;

				h = g;
				g = f;
				f = e;
				e = d + temp1;
				d = c;
				c = b;
				b = a;
				a = temp1 + temp2;
			}

			state[0] += a;
			state[1] += b;
			state[2] += c;
			state[3] += d;
			state[4] += e;
			state[5] += f;
			state[6] += g;
			state[7] += h;
		}

	} // namespace

	std::array<uint8_t, 32> sha256(const uint8_t* data, std::size_t length) {
		uint32_t state[8] = {0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
							 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

		std::size_t offset = 0;
		while (length - offset >= 64) {
			compress(state, data + offset);
			offset += 64;
		}

		// Final block or blocks: the remainder, a 0x80 byte, zero padding, and the bit length.
		uint8_t           tail[128] = {};
		const std::size_t remainder = length - offset;
		std::memcpy(tail, data + offset, remainder);
		tail[remainder] = 0x80;

		const std::size_t tail_length = remainder < 56 ? 64 : 128;
		const uint64_t    bit_length  = static_cast<uint64_t>(length) * 8;
		for (std::size_t index = 0; index < 8; index++) {
			tail[tail_length - 1 - index] = static_cast<uint8_t>((bit_length >> (8 * index)) & 0xFF);
		}

		compress(state, tail);
		if (tail_length == 128) {
			compress(state, tail + 64);
		}

		std::array<uint8_t, 32> digest{};
		for (std::size_t index = 0; index < 8; index++) {
			digest[index * 4]       = static_cast<uint8_t>((state[index] >> 24) & 0xFF);
			digest[(index * 4) + 1] = static_cast<uint8_t>((state[index] >> 16) & 0xFF);
			digest[(index * 4) + 2] = static_cast<uint8_t>((state[index] >> 8) & 0xFF);
			digest[(index * 4) + 3] = static_cast<uint8_t>(state[index] & 0xFF);
		}
		return digest;
	}

	std::string sha256_hex(std::string_view text) {
		const std::array<uint8_t, 32> digest = sha256(reinterpret_cast<const uint8_t*>(text.data()), text.size());

		static constexpr char HEX[] = "0123456789abcdef";
		std::string           out;
		out.reserve(64);
		for (const uint8_t byte : digest) {
			out.push_back(HEX[(byte >> 4) & 0x0F]);
			out.push_back(HEX[byte & 0x0F]);
		}
		return out;
	}

	uint64_t derive(uint64_t master, std::string_view label) {
		std::vector<uint8_t> input;
		input.reserve(NAMESPACE_PREFIX_LENGTH + 8 + label.size());
		input.insert(input.end(), NAMESPACE_PREFIX, NAMESPACE_PREFIX + NAMESPACE_PREFIX_LENGTH);
		for (std::size_t index = 0; index < 8; index++) {
			input.push_back(static_cast<uint8_t>((master >> (8 * (7 - index))) & 0xFF));
		}
		input.insert(input.end(), label.begin(), label.end());

		const std::array<uint8_t, 32> digest = sha256(input.data(), input.size());

		uint64_t value = 0;
		for (std::size_t index = 0; index < 8; index++) {
			value = (value << 8) | digest[index];
		}
		return value;
	}

	uint64_t SplitMix64::next() noexcept {
		state_ += 0x9E3779B97F4A7C15ULL;
		uint64_t z = state_;
		z          = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
		z          = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
		return z ^ (z >> 31);
	}

	uint64_t SplitMix64::next_below(uint64_t bound) noexcept {
		if (bound == 0) {
			return 0;
		}
		return next() % bound;
	}

} // namespace seed
