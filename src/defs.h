#ifndef DEFS_H
#define DEFS_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

// Routing module ids run from 1 to MAX_MODULES - 1.
inline constexpr uint8_t MAX_MODULES = 25;

// The platform accepts CMD_SET_FLIGHT_SCHEDULE only from this id.
inline constexpr uint8_t TOWER_MODULE_ID = 255;

// Sized to cover the tower module id, not just the routing range.
inline constexpr std::size_t MODULE_TABLE_SIZE = 256;

inline constexpr std::chrono::duration TIMEOUT = std::chrono::milliseconds(5);

enum class ModuleLocation {
	LEFT,
	MIDDLE,
	RIGHT,
};

inline constexpr std::size_t RFID_UID_SIZE_BYTES  = 7;
inline constexpr std::size_t RFID_PAGE_SIZE_BYTES = 4;
// An NTAG213 has 36 user-memory pages. The platform reserves the first four and exposes the
// remaining pages to applications with zero-based indexes 0 through 31.
inline constexpr std::size_t RFID_PAYLOAD_PAGE_COUNT = 32;

using RfidUid     = std::array<uint8_t, RFID_UID_SIZE_BYTES>;
using RfidPage    = std::array<uint8_t, RFID_PAGE_SIZE_BYTES>;
using RfidPayload = std::array<RfidPage, RFID_PAYLOAD_PAGE_COUNT>;

#endif
