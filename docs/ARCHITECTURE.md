# Simulator Architecture Guide

## Table of Contents

1. [Project Overview](#1-project-overview)
1. [Software Architecture Overview](#2-software-architecture-overview)
1. [File Structure](#3-file-structure)
1. [Events](#4-events)
1. [Tubs](#5-tubs)
1. [Modules](#6-modules)
1. [Visualization](#7-visualization)

## 1. Project Overview

The simulator intends to simulate running multiple instances of the same router code on the real-world model, allowing for easier debugging and faster iteration. 

A GUI is provided which shows the current state of all modules and actuators. Additionally, the GUI contains tools such as deterministic control over events such as tub or plane arrivals and data through the `events.toml` file, a (real-time) modifiable simulation speed (including pausing), a modifiable layout through the `layout.toml` file, and easy access to actuator values.


## 2. Software Architecture Overview

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Simulation                                                               │
│                                                                          │
│           (updates)       ┌───────────────────┐     (starts m threads)   │
│            ┌──────────────┤        main       ├────────────────┐         │
│            │              └──────┬──────┬─────┘                │         │
│            │                     │      │                      │         │
│            │                     │      │                      │         │
│            │  (starts as thread) │      │ (starts n threads)   │         │
│            │                     │      │                      │         │
│            │                     │      │                      │         │
│  ┌─────────▼─────┐ ┌─────────────▼─┐ ┌──┴──────────┐ ┌─────────┴──────┐  │
│  │ Visualization │ │   TCP Server  │ │     Tub     │ │     Module     │  │
│  └───────────────┘ └─────────┬─────┘ └─────────────┘ └────────────┬───┘  │
│                              │                                    │      │
└──────────────────────────────┼────────────────────────────────────┼──────┘
                               │                                    │
                 (communicate) │                (starts as process) │
                               │                                    │
           ┌───────────────────┼────────────────────────────────────▼─────┐
           │ Module            │                                          │
           │                   │                                          │
           │  ┌────────────────┴──┐               ┌────────────────────┐  │
           │  │    TCP Client     ◀───────────────┤        main        │  │
           │  └───────────────────┘               └────────────────────┘  │
           │                                                              │
           └──────────────────────────────────────────────────────────────┘
```

## 3. File Structure

```
simulation/
├── src/
│   ├── CMakeLists.txt            # Simulation build instructions
│   ├── main.cpp                  # Entry point, event loop, module startup, and shutdown handling
│   ├── defs.h                    # Shared definitions and constants
│   ├── util.cpp/.h               # General utility helpers used across the simulator
│   ├── settings.cpp/.h           # Settings file parsing and accessors
│   │
│   ├── event/                    # Event scheduling and dispatch
│   │   ├── events.cpp            # Event parsing and timed event firing
│   │   └── events.h              # Event model declarations
│   │
│   ├── message/                  # Message types and serialization helpers
│   │   ├── message.cpp           # Message construction and encoding logic
│   │   └── message.h             # Message type declarations
│   │
│   ├── module/                   # Module process lifecycle and registry
│   │   └── module.cpp/.h         # Module process management and coordination
│   │   └── types.h               # Data types used in tubs
│   │
│   ├── tcp/                      # TCP client/server transport layer
│   │   ├── server.cpp/.h         # Server accept loop and connection tracking
│   │   └── socket.h              # Shared socket wrapper and helpers
│   │
│   ├── tub/                      # Tub simulation and routing behavior
│   │   └── tub.cpp/.h            # Tub state updates and movement logic
│   │   └── types.h               # Data types used in tubs
│   │
│   ├── visualization/            # SDL-based simulator UI
│   │   ├── buttons.cpp           # UI button handling and actions
│   │   ├── buttons.h             # Button declarations
│   │   └── visualization.cpp/.h  # Rendering loop and scene drawing
│   │
│   └── synchronization/          # Time coordination helpers
│       └── virtual_clock.h       # Virtual simulation clock abstraction
│
├── test/                         # Unit and integration tests
│   ├── CMakeLists.txt            # Test build instructions
│   │
│   ├── event/                    # Event tests
│   ├── message/                  # Message tests
│   ├── module/                   # Module tests
│   ├── synchronization/          # Synchronization tests
│   ├── tcp/                      # TCP tests
│   └── tub/                      # Tub tests
│
├── .clang-format                 # The code style guidelines
├── .gitignore                    # Ignored Git files
├── .gitlab-ci.yml                # GitLab CI runner details
├── CMakeLists.txt                # Main build script for the application and tests
├── README.md                     # Project overview
├── events_example.toml           # Example event schedule input
├── layout_example.toml           # Example layout
└── settings_example.toml         # Example simulator configuration input
```

## 4. Events

Events are stored in the `events.toml` file and represent the real-world actions that you can perform with tubs. These events have a time of occurrence in seconds, additionally supporting specifying milliseconds. 

At the start, we collect all events. Then, every 16 milliseconds, we loop over all events and check whether an event should be fired. If an event should be fired, we run the event-specific code for announcing to the modules that an event has occurred.

There are two types of events:

#### Tub arrivals
This is the arrival of a tub into the system at a specific module or at a specific plane. Each tub arrival has fields which populate the minimum amount of information required for tub routing to work. These are: the origin module or plane id, the destination module or plane id, whether this tub is safe (used for things like security) and whether this tub needs to go to security. This necessary data takes up the first two rows of each RFID chip. The remaining 33 rows of RFID data can be manually specified.

#### Plane arrival
This is the arrival of a plane at a specific module and at a specific time.

## 5. Tubs

## 6. Modules

## 7. Visualization