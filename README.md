# Quercus Simulation
The repository for the Quercus Airport Router Simulation.

## Requirements
- Ubuntu 24 or higher, Debian 13 or higher
- CMake, Make
- Internet connection

## Installation
1. Clone the router repo locally. Set up Zephyr and West. If done correctly, you should be able to build a native `zephyr.exe` file using the run/build script present.
2. Clone this repo locally. 
3. Rename `events_example.toml` to `events.toml` and rename `settings_example.toml` to `settings.toml`.
4. Change the `router_path` value in the settings to the absolute path of the `zephyr.exe` executable.

## Usage
Run the project with the `Run` button when using the C/C++ VSCode extension, or use: 

```sh
make && ./main
```

## Support
In case you encounter any problems, send an email to `r.verstappen@student.tue.nl`. This email should contain as much relevant information as possible, including your implementation of the router.

## Contributing
If you make improvements to the digital simulation for your project, you are free to contribute these changes to this repo. Any merge requests will be thoroughly reviewed before merging, so make sure you are willing to change your code before opening a merge request.

## License
(c) Eindhoven University of Technology, Rens Verstappen, and contributors (2025-2026).
