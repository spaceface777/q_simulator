# Simulation Architecture Guide

## Table of contents

1. [Project Overview](#1-project-overview)
1. [Software Architecture Overview](#2-software-architecture-overview)
1. [File Structure](#3-file-structure)
1. [Events](#4-events)
1. [World](#5-world)
1. [Applications](#6-applications)
1. [Firmware transport](#7-firmware-transport)
1. [Visualization](#8-visualization)
1. [Questions or feedback](#9-questions-or-feedback)

## 1. Project overview

The simulator runs Quercus applications against a deterministic model of the airport-baggage modules.
A world executable contains the layout and one private Zephyr `native_sim` image for each active module.
The simulator supplies the platform RPC service that the application uses on hardware.

The layout defines the modules, their connections, and their firmware.
The scenario defines the duration, timed events, and LTLf properties.
An optional settings file overrides the runtime defaults.

Simulated time advances in discrete 1 ms ticks. One master seed controls application seeds, physical calibration, start offsets, clock drift, skipped local ticks, network delay, network loss, and generated RFID data. The same inputs and seed produce the same trace.

## 2. Software architecture overview

A world executable runs one scenario in one process.
The runtime selects modules for each global tick and calls their firmware images with absolute time grants.
Each image advances its own clock and returns its RPC frames to the runtime.

The runtime prepares all grants before it processes any output.
The task queue then orders RPC requests, network deliveries, world motion, sensor changes, and property observations.
The trace records the run metadata and observations.

## 3. File structure

```
simulation/
├── src/
│   ├── defs.h                    # Shared constants
│   ├── settings.cpp/.h           # Settings and layout parsing
│   ├── util.cpp/.h               # Common parsing and validation helpers
│   │
│   ├── firmware/                 # World assembly and firmware grants
│   ├── engine/                   # Deterministic tick task queue
│   ├── event/                    # Timed world-event and scenario input
│   ├── ltl/                      # LTLf lexer, parser, checker, monitor, and observer
│   ├── module/                   # Module state and strongly typed values
│   ├── net/                      # Deterministic network delay and loss
│   ├── platform/                 # Hardware and network RPC services
│   ├── rpc/                      # RPC frame encoding and validation
│   ├── scenario/                 # Headless scenario entry point
│   ├── seed/                     # Named random streams from the master seed
│   ├── sim/                      # Lockstep runtime and speed reporting
│   ├── synchronization/          # Interactive virtual clock
│   ├── trace/                    # NDJSON trace writer
│   ├── visualization/            # SDL3 controls and drawing
│   └── world/                    # Physical state and calibrated behavior
│
├── test/                         # Unit, world, and applet tests
│   └── applets/                  # Test applets with their scenarios, properties, and expected output
├── editors/vscode-ltlf/         # VS Code support for the LTLf property language
├── q_sdk/                        # Pinned Quercus SDK
├── CMakeLists.txt                # Application and test build
└── settings_example.toml         # Example run and physical settings
```

## 4. Events

The scenario file contains the timed events.
A tub event gives the time, tub ID, module ID, optional position, optional removal time, and seven-byte RFID UID.
A tub event can also supply up to 32 four-byte payload pages.
The simulator generates missing payload pages from the master seed.

The `[scenario]` table gives the duration and can name an LTLf property file.
A relative property path starts at the scenario file directory.
Without properties, the run ends after the inclusive duration tick.
The runtime places tick-zero tubs before the first firmware grant and applies later events in tick order.

## 5. World

`world::State` owns every module and tub. It advances all tubs once per simulated tick in ascending tub-id order. Positions use integer micrometres. A remainder keeps sub-micrometre belt travel between ticks. This makes slow motion deterministic and prevents rounding from stopping a tub.

Each module has a calibrated big belt, small belt, servo, lasers, RFID reader, LED, and current-voltage model. A module receives one deterministic calibration from the physical population. The model derives belt travel, encoder values, sensor state, voltage, current, and power from the current commands and calibration.

A tub moves on the big or small belt. The servo selects its path at the junction. The layout gives the left, middle, and right neighbor of each routing module. A hand-off occurs only when the destination endpoint can receive the tub. RFID writes update the tub in the world, so the new page remains present after the next sensor update.

## 6. Applications

World assembly links one private image for each active layout module.
The runtime binds images in ascending module ID order.
The first heartbeat changes a module from activating to running.
A stopped module receives no further grants.
Module 255 is the optional tower and has no conveyor geometry.

Each physical module has an independent start offset, fixed clock rate, and local tick-skip probability. The global world and network clock still advances on every simulator tick. A skipped local tick does not advance that application's uptime, code, RPC service delays, or rate-limit windows. A ready response or event waits for the module's next executed tick.

## 7. Firmware transport

The image ABI exchanges RPC frames through bounded buffers.
Each buffer holds four frames and up to 4088 data bytes.
A frame can use at most 1024 bytes.
The simulator validates the frame offsets before it reads the output.

The runtime skips an idle grant when the image has no input and its next wake is beyond the grant.
The image catches up to the start of the next grant before it receives input.

`docs/PROTOCOL.md` defines the RPC frame. Requests cross the next application handoff and enter one of three deterministic Core0 handler lanes owned by that module. Lane assignment follows seeded intake order. A confirmable request keeps its lane through response handoff even when it is waiting for that module's per-category mutex. Different categories on one module and matching categories on separate modules can therefore run concurrently, while one module's category remains serialized. Responses and events return on an executed local tick. Laser events have a configurable additional delay. RFID events use a configurable seeded delay distribution. Module packets, logs, and panic reports use the configured deterministic network delay and loss model.

## 8. Visualization

The world currently runs headlessly.
The visualization sources remain available for the P8 SDL3 integration.

## 9. Questions or feedback

Create a GitLab issue if you find a problem. Use the provided issue format.
