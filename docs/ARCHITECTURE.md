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

A GUI is provided which shows the current state of all modules and actuators. Additionally, the GUI contains tools such as deterministic control over events such as tub or plane arrivals and data through the `events.toml` file, a (real-time) modifiable simulation speed (including pausing), changing connections between modules through the `settings.toml` file, a modifiable layout, and easy access to actuator values.


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
│   ├── main.cpp                  # Entry point, event loop, module startup, and shutdown handling
│   ├── defs.h                    # Shared definitions and constants
│   ├── util.cpp/.h               # General utility helpers used across the simulator
│   ├── settings.cpp/.h           # Settings file parsing and accessors
│   │
│   ├── lib/                      # Third-party or embedded library sources
│   │   └── toml.h                # Single-header TOML parser
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
│   │
│   ├── tcp/                      # TCP client/server transport layer
│   │   ├── client.cpp/.h         # Client connection management
│   │   ├── server.cpp/.h         # Server accept loop and connection tracking
│   │   └── socket.h              # Shared socket wrapper and helpers
│   │
│   ├── tub/                      # Tub simulation and routing behavior
│   │   └── tub.cpp/.h            # Tub state updates and movement logic
│   │
│   ├── visualization/            # SDL-based simulator UI
│   │   ├── buttons.cpp           # UI button handling and actions
│   │   ├── buttons.h             # Button declarations
│   │   └── visualization.cpp/.h  # Rendering loop and scene drawing
│   │
│   └── synchronization/          # Time coordination helpers
│       └── virtual_clock.h       # Virtual simulation clock abstraction
│
├── test/                         # Unit tests
│   ├── CMakeLists.txt            # Test target definition and registration
│   └── test_message.cpp          # Message-layer unit tests
│
├── .gitlab-ci.yml                # GitLab CI runner details
├── events_example.toml           # Example event schedule input
├── README.md                     # Project overview
├── settings_example.toml         # Example simulator configuration input
└── CMakeLists.txt                # Main build script for the application and tests
```

## 4. Events

## 5. Tubs

## 6. Modules

## 7. Visualization