#ifndef DEFS_H
#define DEFS_H

#include <chrono>

/**
 * The max amount of modules supported.
 */
inline constexpr uint8_t MAX_MODULES = 25;

/**
 * Max size for TCP messages.
 */
inline constexpr std::size_t MAX_TCP_BYTES_SIZE = 512;

/**
 * Max TCP backlog.
 */
inline constexpr int MAX_TCP_BACKLOG = 128;

/**
 * A constant timeout value.
 */
inline constexpr std::chrono::duration TIMEOUT = std::chrono::milliseconds(5);

enum class ModuleLocation
{
    LEFT,
    MIDDLE,
    RIGHT,
};

inline constexpr std::size_t RFID_UID_SIZE_BYTES = 7;
inline constexpr std::size_t RFID_BLOCK_SIZE_BYTES = 4;
inline constexpr std::size_t RFID_CONTENT_BLOCK_COUNT = 36;

#endif