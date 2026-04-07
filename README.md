# Quercus Simulation
The repository for the Quercus Airport Router Simulation.

## Requirements
- Ubuntu 24 or higher, Debian 13 or higher
- Internet connection

## Installation
1. Run `sudo apt install build-essential libsdl2-dev git`.
2. Clone the router repo locally. Set up Zephyr and West. If done correctly, you should be able to build a native `zephyr.exe` file using the run/build script present.
3. Clone this repo locally. 
4. Rename `events_example.toml` to `events.toml` and rename `settings_example.toml` to `settings.toml`.
5. Change the `router_path` value in the settings to the absolute path of the `zephyr.exe` executable.
6. Run `cmake build/ -DCMAKE_POLICY_VERSION_MINIMUM=3.5`.

## Usage
Run the project with the `Run` button when using the C/C++ VSCode extension, or use: 

```sh
make -C build -j && ./build/main
```

If any errors appear about addresses already being in use, make sure to kill any potential runaway Zephyr processes.
You can usually find these in a task manager or system monitor.

## Support
In case you encounter any problems, send an email to `r.verstappen@student.tue.nl`. This email should contain as much relevant information as possible, including your implementation of the router.

## Contributing
If you make improvements to the digital simulation for your project, you are free to contribute these changes to this repo. Any merge requests will be thoroughly reviewed before merging, so make sure you are willing to change your code before opening a merge request.

## License
(c) Eindhoven University of Technology, Rens Verstappen, and contributors (2025-2026).
