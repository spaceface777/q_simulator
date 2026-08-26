# Simulation Architecture Guide

## Table of contents

1. [Project Overview](#1-project-overview)
1. [Software Architecture Overview](#2-software-architecture-overview)
1. [File Structure](#3-file-structure)
1. [Events](#4-events)
1. [World](#5-world)
1. [Applications](#6-applications)
1. [Shared memory transport](#7-shared-memory-transport)
1. [Visualization](#8-visualization)
1. [Questions or feedback](#9-questions-or-feedback)

## 1. Project overview

The simulator runs the real Quercus applications against a deterministic model of the airport-baggage modules. Each application is a Zephyr `native_sim` child process. The simulator supplies the platform RPC service that the application uses on the hardware.

The GUI shows all modules, tubs, sensors, and actuators. Input files define the module layout, application files, physical settings, network settings, master seed, and timed tub events. A headless runner checks finite scenarios against LTLf properties and returns a process status for CI.

Simulated time advances in discrete 1 ms ticks. One master seed controls application seeds, physical calibration, start offsets, clock drift, skipped local ticks, network delay, network loss, and generated RFID data. The same inputs and seed produce the same trace.

## 2. Software architecture overview

The process and component placement is:

```text
Simulation run
|-- Simulation process
|   |-- Front end (one per run)
|   |   |-- Interactive main, or
|   |   `-- Headless scenario runner
|   |-- Lockstep runtime
|   |   |-- Deterministic task queue
|   |   |-- World state
|   |   |-- Platform manager
|   |   |   |-- Per-module platform state
|   |   |   |-- Simulated module network
|   |   |   `-- Simulated diagnostic network
|   |   |-- Shared control mapping
|   |   |-- Child state (one per active module)
|   |   |   |-- Child process handle
|   |   |   `-- Shared mailbox mapping
|   |   |-- Property observer
|   |   |-- LTLf monitors (one per property)
|   |   `-- Trace writer
|   `-- Visualization (interactive runs only)
`-- native_sim child process (one per active module)
    |-- Quercus application
    `-- SDK lockstep adapter
        |-- View of the shared control mapping
        `-- View of that child's shared mailbox mapping
```

The directed relationships are:

```text
Interactive main -- [constructs and drives] -> Lockstep runtime
Interactive main -- [updates] -> Visualization
Headless scenario runner -- [constructs and drives] -> Lockstep runtime

Settings and layout -- [configure] -> Lockstep runtime
Event schedule -- [loads timed world changes into] -> Lockstep runtime
LTLf property file -- [loads property declarations into] -> Lockstep runtime

Lockstep runtime -- [schedules ordered tick phases on] -> Deterministic task queue
Lockstep runtime -- [owns and advances] -> World state
Lockstep runtime -- [owns and calls] -> Platform manager
Lockstep runtime -- [launches and supervises] -> native_sim child process
Lockstep runtime -- [grants ticks through] -> Shared control mapping
Lockstep runtime -- [writes responses and events to] -> Shared mailbox mapping
Shared mailbox mapping -- [provides application requests to] -> Lockstep runtime

Platform manager -- [reads and updates hardware state in] -> World state
Platform manager -- [owns and advances] -> Simulated module network
Platform manager -- [owns and advances] -> Simulated diagnostic network
Lockstep runtime -- [dispatches decoded RPC requests to] -> Platform manager
Platform manager -- [provides RPC responses and events to] -> Lockstep runtime

Lockstep runtime -- [submits world and RPC facts to] -> Property observer
Property observer -- [builds tick observations for] -> LTLf monitors
Lockstep runtime -- [progresses and finishes] -> LTLf monitors
Lockstep runtime -- [writes run metadata and tick records through] -> Trace writer

Visualization -- [reads] -> World state
Quercus application -- [calls platform RPC through] -> SDK lockstep adapter
SDK lockstep adapter -- [waits for tick grants on] -> Shared control mapping
SDK lockstep adapter -- [acknowledges completed grants on] -> Shared control mapping
SDK lockstep adapter -- [reads responses and events from] -> Shared mailbox mapping
SDK lockstep adapter -- [writes application requests to] -> Shared mailbox mapping
```

At each barrier, the runtime selects the active applications and grants their next local tick. Each child reads inbound frames, runs its application code, writes outbound frames, and acknowledges the barrier. The simulator waits for all live children. It then applies RPC requests, network deliveries, world motion, sensor changes, events, trace output, and property observations in a fixed order.

The default barrier grants one tick. A larger batch grants up to 16 ticks at one barrier. Each batch entry has separate inbound and outbound mailboxes. The simulator can observe and answer an application action only at the batch boundary, so a batch of N makes N ms the smallest effective response step. A batch of 10 runs about seven times faster than a batch of 1. The exact gain depends on the CPU, layout, scenario, and application.

## 3. File structure

```
simulation/
├── src/
│   ├── main.cpp                  # Interactive entry point and render loop
│   ├── defs.h                    # Shared constants
│   ├── settings.cpp/.h           # Settings and layout parsing
│   ├── util.cpp/.h               # Common parsing and validation helpers
│   │
│   ├── childproc/                # native_sim process launch and cleanup
│   ├── childsync/                # Shared mappings, futex barrier, and mailboxes
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
├── test/                         # Unit, integration, and scenario tests
├── editors/vscode-ltlf/         # VS Code support for the LTLf property language
├── external/                     # Pinned quercus_applets and quercus_sdk
├── layouts/                      # Example layouts
├── scenarios/                    # Scenario TOML files and LTLf property files
├── CMakeLists.txt                # Application and test build
├── events_example.toml           # Example timed world events
├── layout_example.toml           # Example module graph and geometry
└── settings_example.toml         # Example run and physical settings
```

## 4. Events

The simulator reads timed input from `events.toml` or from a scenario TOML file. A tub event gives the tick, tub id, module id, optional position, optional removal time, seven-byte RFID UID, and up to 32 four-byte payload pages. Missing payload pages contain deterministic data from the master seed.

An event changes the physical world. It does not send a message to an application. The application observes a tub through the laser and RFID RPC services when the tub reaches those instruments.

A finite scenario also has a `[scenario]` table. Its `duration` sets the inclusive end of the trace. Its `properties` value names the LTLf file. The runtime applies tick-zero events before it starts the child processes, and it applies later events in tick order.

## 5. World

`world::State` owns every module and tub. It advances all tubs once per simulated tick in ascending tub-id order. Positions use integer micrometres. A remainder keeps sub-micrometre belt travel between ticks. This makes slow motion deterministic and prevents rounding from stopping a tub.

Each module has a calibrated big belt, small belt, servo, lasers, RFID reader, LED, and current-voltage model. A module receives one deterministic calibration from the physical population. The model derives belt travel, encoder values, sensor state, voltage, current, and power from the current commands and calibration.

A tub moves on the big or small belt. The servo selects its path at the junction. The layout gives the left, middle, and right neighbor of each routing module. A hand-off occurs only when the destination endpoint can receive the tub. RFID writes update the tub in the world, so the new page remains present after the next sensor update.

## 6. Applications

The runtime starts one `native_sim` child process for each active layout module. `application_path` supplies the common file. A module can use its own `binary` value. Module 255 is the optional tower and has no conveyor geometry.

The child receives its module id and inherited shared-memory descriptors in `QSIM_MODULE_ID`, `QSIM_CTRL_FD`, and `QSIM_MBOX_FD`. It validates the shared-memory ABI, maps its private mailbox segment, marks itself ready, and waits for the next barrier generation. The first heartbeat changes its lifecycle from activating to running.

Each physical module has an independent start offset, fixed clock rate, and local tick-skip probability. The global world and network clock still advances on every simulator tick. A skipped local tick does not advance that application's uptime, code, RPC service delays, or rate-limit windows. A ready response or event waits for the module's next executed tick.

## 7. Shared memory transport

The simulator and SDK use the same `quercus/lockstep_shm.h` layout. One control mapping holds the ABI, barrier generation, completion state, per-module tick index, tick duration, batch size, active flags, and ready flags. Each child has a separate segment with inbound and outbound mailboxes for every tick in a batch.

A mailbox is one 4096-byte page. It has four frame slots and 4088 data bytes. A frame can use at most 1024 bytes. The simulator validates every slot offset before it reads child data. It returns busy or no-memory status when a frame cannot fit.

The barrier uses acquire and release operations and Linux futex waits. The parent writes mailboxes only while children are blocked. A child writes its outbound mailbox only while the parent is blocked. The final child increments the release word and wakes the parent.

`docs/PROTOCOL.md` defines the RPC frame. Requests cross the next application handoff and enter one of three deterministic Core0 handler lanes owned by that module. Lane assignment follows seeded intake order. A confirmable request keeps its lane through response handoff even when it is waiting for that module's per-category mutex. Different categories on one module and matching categories on separate modules can therefore run concurrently, while one module's category remains serialized. Responses and events return on an executed local tick. Laser events have a configurable additional delay. RFID events use a configurable seeded delay distribution. Module packets, logs, and panic reports use the configured deterministic network delay and loss model.

## 8. Visualization

The SDL3 visualization reads the runtime world after each engine advance. It draws the configured module positions and angles, current actuator values, sensor state, tubs, trace status, simulation time, and effective speed in a 2D top-down view.

The interactive loop targets 60 frames per second. It reserves 2 ms of each frame for input and drawing and gives the remaining time to child progress. The simulation clock shown in the GUI is the engine tick that the applications experienced.

The controls can pause the run, change the requested simulation speed, and change the view scale. The default scale comes from `settings.toml`. Closing the window stops the runtime and all child processes.

## 9. Questions or feedback

Create a GitLab issue if you find a problem. Use the provided issue format.
