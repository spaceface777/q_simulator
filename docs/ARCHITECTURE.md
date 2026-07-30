# Simulation Architecture Guide

## Table of contents

1. [Project Overview](#1-project-overview)
1. [Software Architecture Overview](#2-software-architecture-overview)
1. [File Structure](#3-file-structure)
1. [Events](#4-events)
1. [Tubs](#5-tubs)
1. [Modules](#6-modules)
1. [TCP server](#7-tcp-server)
1. [Visualization](#8-visualization)
1. [Questions or feedback](#9-questions-or-feedback)

## 1. Project overview

The simulation intends to simulate running multiple instances of the same module code on the real-world model, allowing for easier debugging and faster iteration. 

A GUI is provided which shows the current state of all modules and actuators. Additionally, the GUI contains tools such as deterministic control over events such as tub or plane arrivals and data through the `events.toml` file, a (real-time) modifiable simulation speed (including pausing), a modifiable layout through the `layout.toml` file, and easy access to actuator values.


## 2. Software architecture overview

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

## 3. File structure

```
simulation/
├── src/
│   ├── CMakeLists.txt            # Simulation build instructions
│   ├── main.cpp                  # Entry point, event loop, module startup, and shutdown handling
│   ├── defs.h                    # Shared definitions and constants
│   ├── util.cpp/.h               # General utility helpers used across the simulation
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
│   ├── visualization/            # SDL-based simulation UI
│   │   ├── buttons.cpp           # UI button handling and actions
│   │   ├── buttons.h             # Button declarations
│   │   └── visualization.cpp/.h  # Rendering loop and scene drawing
│   │
│   └── synchronization/          # Time coordination helpers
│       └── virtual_clock.h       # Virtual simulation clock abstraction
│
├── test/                         # Unit and integration tests
│   ├── CMakeLists.txt            # Test build instructions
│   ├── test_settings.cpp         # Tests for src/settings.cpp
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
├── CMakeLists.txt                # General build instructions
├── README.md                     # Project overview
├── events_example.toml           # Example event schedule input
├── layout_example.toml           # Example layout
└── settings_example.toml         # Example simulation configuration input
```

## 4. Events

Events are stored in the `events.toml` file and represent the real-world actions that you can perform with tubs. After parsing, events are stored in `main.cpp` as classes that have an id (which may be a module id or plane id), and a time of occurrence in seconds, additionally supporting specifying milliseconds. There are two types of events:

- **Tub arrival**: This is the arrival of a tub into the system at a specific module or at a specific plane. Each tub arrival has fields which populate the minimum amount of information required for tub routing to work. These are: the origin module or plane id, the destination module or plane id, whether this tub is safe (used for things like security) and whether this tub needs to go to security. This necessary data takes up the first two rows of each RFID chip. The remaining 33 rows of RFID data can be manually specified.
- **Plane arrival**: This is the arrival of a plane at a specific module and at a specific time. The only data this event has, other than the plane id, is the amount of time left until this plane departs.

We process these two events as follows: at the start, we collect all events from `events.toml`. All tub arrival events are sent as a packet to all modules to populate the schedule of these modules. This allows users to run any `events.toml` without having to change the module code. 

Then, every 16 milliseconds, we loop over all events and check whether an event should be fired. If an event should be fired, we run the event-specific code for announcing to the modules that an event has occurred. For tub arrivals, this means sending a packet to the modules which updates the values present at the RFID reader. For plane arrivals, this means sending a packet to all modules which indicates that a plane has arrived.

## 5. Tubs

Tubs are classes which keep track of tub data, additionally including a thread for each tub. This thread continuously update the position of the tub based on the state of the module the tub belongs to. 

When a tub is within the range of a sensor, it sends a packet to the owning module that the tub is present. For lasers, this is done using the value `true`. For the RFID chip, it sends all RFID data. When the tub exits the RFID chip area, it sends a packet with empty RFID data. For exiting the lasers, `false` is sent.

When a tub should transfer from one belt part to the other on the same module, the value of the servo is read. Based on this, the correct belt is chosen. For transferring to other modules, the connections found in `layout.toml` are checked. Only the tub's owning module variable is updated; modules do not keep track of the tubs they own.

## 6. Modules

Modules are classes that keep track of data received from each module process. They are responsible for starting their respective module processes, which is the router code compiled for native_sim. This is done by creating a new thread, and using this thread to start the router code executable (called the module process) with `execlp`. Since we cannot pass command-line arguments to native_sim executables, we create a temp file before starting the module process and write the id of this module process in it. This gets read by the module process, and the file is deleted.

## 7. TCP server

The TCP server establishes and maintains connections between the simulation and every module process. All communication happens in packets specified in `docs/PROTOCOL.md`. We check whether there are new packets to process every 5 milliseconds. 

When a sensor needs to be updated due to a tub being present, the simulation sends a packet to the module process. When an actuator is updated by a module, a packet is sent to the simulation, and the correct value in the module class is updated.

Communication between modules happens through the simulation. When module A wants to send something to module B, it sends a `[packet]` packet to the simulation, which then sends the packet to module B. 

## 8. Visualization

The visualization takes the existing state of all tubs and modules and visualizes this in a 2D top-down view, using SDL3, and runs on the main thread. We update every 16 milliseconds, giving a target FPS of 62.5. We do not account for execution time, as this is at most 0.5 milliseconds, resulting in an average FPS of 61. 

The `layout.toml` file contains the position of all modules and angle of all modules, which are used to render each module. For each module, we use specific offsets at specific angles to render all elements properly. Every update, we check the current state of each module, and based on it, update the visualization parameters, such as the servo angle, belt speed, etc. For tubs, we use these specific offsets for each module to render their location correctly. Rendering tubs consists of getting the (x, y)-location of the module, and, based on the belt of the tub, rendering the circle. 

Scaling works by using a wrapper function for all constant offsets in a module. All offsets were chosen with a module size of 180x180 pixels in mind, which is a scaling of 1.0. Each offset is multiplied by the current scaling, which reduces or increases size.

All buttons snap to a specific corner of the screen and are specified by an offset. These check for click events every 16 milliseconds, and, if one has occurred, performs the correct action. The pause button keeps track of the previous value, allowing pausing and playing. The current simulation speed, time and scaling are also buttons, but these have no functionality. 

For tracking time with simulation speed, we use a virtual clock which ticks every 16 milliseconds. Each tick increments the time based on the value of the simulation speed setting. Since we do not allow simulation speed updates between ticks, this clock remains accurate.

## 9. Questions or feedback

In case you encounter any problems, create an issue on GitLab. Please use the provided issue format.
