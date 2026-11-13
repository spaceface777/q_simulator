# Cosmopolitan P5 verification

P5 ports the preview optimizations and the platform RPC changes to linked worlds.
The simulator uses historical P5 applet sources from `cfcdddc`.
The applet repository changes only its SDK reference.
P5 uses firmware ABI version 2.

## Router and LTLf verdicts

The P5 simulator and `windows-llvm` at `bdbcfae` gave the same 71 router case verdicts on macOS arm64 and Linux x86-64.
Both passed 60 cases, skipped four, and failed these seven cases.

- `course__multi_corridor`
- `course__multi_proved`
- `course__storage_full`
- `goal4__refuse_midpath`
- `regression__failure_019_gate_admission`
- `regression__failure_022_security_return`
- `regression__failure_228_active_reroute`

On Linux, both simulators reported the same failed or undecided properties at the same ticks.
Four diagnostic scenarios also matched. Laser alignment and module movement passed.
The two hardware diagnostics each had one undecided RFID property on both simulators.

On macOS and Linux, the eight bundled scenarios reproduced their traces on repeat runs.
Three diagnostic trace bodies also matched across the two hosts after normalization.
The normalization removed checkout path prefixes and differences in the Zephyr boot banner's revision string.
Windows execution is pending.

## MiniPC measurements

The comparison used Linux x86-64 on an Intel N100 and pinned both builds to CPU 2.
The reference used GCC 15 and LTO for the simulator.
P5 used Cosmopolitan GCC 16.2 and LTO for the simulator and Zephyr firmware.
Both used the coroutine backend and disabled PGO.

The router used the existing MiniPC benchmark snapshot on both builds.
Its source matches the tracing fork at `bededba` with the unused parameters removed from `main`.
Each router case used the same settings, layout, seed, and LTLf file in both builds.
The reference loaded firmware images at startup. P5 reused its assembled world.

The times sum the scenario executions in each run. They exclude compilation and world assembly.
The second run reversed the order of the two builds.

| Workload | `windows-llvm` runs (s) | P5 runs (s) | Median reduction |
| --- | ---: | ---: | ---: |
| Router cases | 49.881, 49.100, 49.107 | 35.031, 33.937, 33.098 | 31% |
| Four LTLf diagnostics | 0.658, 0.607, 0.708 | 0.476, 0.358, 0.413 | 37% |

The timing compares the full builds. The compiler, firmware LTO, and image loading all differ between the builds.
