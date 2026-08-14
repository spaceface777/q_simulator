#ifndef SEED_SEED_H
#define SEED_SEED_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace seed {

	// Derives a substream seed from the master seed and a stable label, e.g. "shuffle/12/0/3". The
	// derivation matches the Go implementation byte for byte, so both engines make the same choices
	// for the same seed and label.
	[[nodiscard]] uint64_t derive(uint64_t master, std::string_view label);

	// Matches the Go implementation's generator; chosen for reproducibility, not statistical quality.
	class SplitMix64 {
	private:
		uint64_t state_;

	public:
		explicit SplitMix64(uint64_t state) noexcept : state_(state) {}

		uint64_t next() noexcept;

		// bound must be positive. Returns the next value reduced into [0, bound).
		uint64_t next_below(uint64_t bound) noexcept;
	};

	// Deterministic Fisher-Yates shuffle, matching the Go implementation. The caller must put the
	// sequence into a canonical order first, or the result varies between runs regardless of seed.
	template <typename T> void shuffle(std::vector<T>& values, uint64_t master, std::string_view label) {
		if (values.size() < 2) {
			return;
		}

		SplitMix64 stream(derive(master, label));
		for (std::size_t index = values.size() - 1; index > 0; index--) {
			const std::size_t swap_index = static_cast<std::size_t>(stream.next_below(index + 1));
			std::swap(values[index], values[swap_index]);
		}
	}

	[[nodiscard]] std::array<uint8_t, 32> sha256(const uint8_t* data, std::size_t length);

	// Lowercase hexadecimal.
	[[nodiscard]] std::string sha256_hex(std::string_view text);

} // namespace seed

#endif // SEED_SEED_H
