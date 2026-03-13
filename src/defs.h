#ifndef DEFS_H
#define DEFS_H

#include <chrono>

typedef uint8_t id_t;

/**
 * The max amount of modules supported.
 */
inline constexpr uint8_t MAX_MODULES = 25;

/**
 * The module amount currently being used.
 */
inline constexpr uint8_t MODULE_COUNT = 19;

/**
 * Max size for TCP messages.
 */
inline constexpr uint16_t MAX_TCP_BYTES_SIZE = 512;

/**
 * Max TCP backlog.
 */
inline constexpr uint8_t MAX_TCP_BACKLOG = 25;

/**
 * A constant timeout value.
 */
inline constexpr std::chrono::duration TIMEOUT = std::chrono::milliseconds(50);

enum ModuleLocation
{
    LEFT_BELT,
    RIGHT_BELT,
    BOTTOM_BELT,
};

#endif