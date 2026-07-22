<!-- 
# TODO
**general**
- add synchronization for processes
- analyze implementing scheduler process that communicates to the router processes
- for hybrid mode send through pi, add separate interface for communication
- int -> double for event time
- make id in events.toml 7 byte rfid
- remove server/client and fix tcp

**event generator**
- total amount of tubs, density of tubs/planes
-->

# Quercus Simulation
The repository for the Quercus Airport Simulation. Please read the entire README before starting.

![A screenshot of the simulator GUI](docs/images/gui.png)

## Repository Structure

```
simulation/
├── .gitlab/                 # GitLab issue formats 
├── docs/                    # Architecture overview and diagrams
├── layouts/                 # Example layouts for use in settings.toml
├── src/                     # The source code
├── test/                    # All tests
├── .gitlab-ci.yml           # GitLab CI runner details
├── events_example.toml      # Example event schedule input
├── README.md                # Project overview
├── settings_example.toml    # Example simulator configuration input
└── CMakeLists.txt           # Main build script for the application and tests
```

## Requirements
- Ubuntu 24 or higher / Debian 13 or higher
- Internet connection

## Installation
1. Run `sudo apt install build-essential libsdl2-dev git`.
1. Clone the router repo locally. Set up West and Zephyr, as described **[here](https://gitlab.tue.nl/2irr70-capstone-quercus-airport/quercus_platform_pico2#environment-setup-ci-compatible-baseline)**. If done correctly, you should be able to build a native `zephyr.exe` file using the run/build script present. This file is usually found at `build/zephyr/zephyr.exe`.
1. Clone this repo locally. 
1. Rename `events_example.toml` to `events.toml` and rename `settings_example.toml` to `settings.toml`.
1. Change the `router_path` value in the settings to the absolute path of the `zephyr.exe` executable. The absolute path should look similar to this: `/home/user/airport-router/build/zephyr/zephyr.exe`.
1. Run `cmake -B build -S . -G Ninja`.

## Usage
Run the project with the `Run` button when using the C/C++ VSCode extension, or use the following command in the project folder: 

```sh
cmake --build build -j && ./build/main
```

This will start all modules and run the current events in the `events.toml` file until infinity. Closing the GUI stops the simulation and all modules. Changes to the `events.toml` or `settings.toml` files will only apply after restarting the simulation by closing the GUI and re-running the project as described above.

To run the tests, change the target to `tests` when using the C/C++ VSCode extension, or use the following command in the project folder:

```sh
cmake --build build -j && ./build/tests
```

## Support
In case you encounter any problems, create an issue on GitLab. Please use the provided issue format.

## Contributing
If you make improvements to the digital simulation for your project, you are free to contribute these changes to this repo. Any merge requests will be thoroughly reviewed before merging, so make sure you are willing to change your code before opening a merge request.

## License
(c) Eindhoven University of Technology, Rens Verstappen, and contributors (2025-2026).
