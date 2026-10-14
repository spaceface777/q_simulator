# Firmware image protocol

`quercus/sim_instance.h` defines native ABI version 10.
An image exports one function, `qsim_image_api()`, which returns an ABI version and three function pointers:

- `bind(config)` validates and copies the module ID, seed, and explicit environment snapshot.
- `run(grant, result)` executes an absolute native-time window with a bounded input batch.
- `destroy()` stops the runner and releases image-owned resources.

Each loaded copy holds one module instance. There are no instance tokens or host allocation arenas.
The host checks the ABI version after loading the image. The OS loader checks architecture compatibility.

## Binding and boot

Binding does not execute runner or firmware code.
Boot happens in the first grant, through upstream `nsi_init` with real-time pacing disabled,
the simulated RTC reset, and the configured deterministic seed.
The image restores the host's SIGINT and SIGTERM handlers after initialization.
Input becomes visible after boot; frames produced during boot belong to that grant's output.

The environment consists of explicit `KEY=VALUE` entries copied at binding and a private `QUERCUS_MODULE_ID`.
Host environment variables are unavailable. Repeated lookups return stable values and pointers.
A fresh load creates fresh image state.

## Grants and results

A grant specifies `start_us`, `until_us`, and input frame pointers.
Grant windows must advance time and cannot overlap completed execution.
If native time is behind `start_us`, the image first catches up without exposing input.
It then advances through `until_us`, including native events at that exact timestamp.

A result supplies an outcome, next wake time, output frame pointers, and optional diagnostic text.
Output pointers remain valid only until another image operation changes their storage.
The host copies output before executing another module and consumes all output after the epoch completes.
With no input and a next wake time beyond the window, the host can skip execution.

Image API status codes report invalid arguments, resource limits, or transport failures.
A native fatal outcome reports firmware termination without terminating the simulator.
Non-yielding firmware cannot be preempted. Destroy failures are reported to the caller or stderr during cleanup.

## RPC frames

`quercus/rpc_wire.h` defines the packed eight-byte header and payload limit.
The maximum complete frame is 1024 bytes. Payloads up to `RPC_MAX_PAYLOAD` are valid.
`quercus/sim_transport.h` defines frame-count and byte limits for each grant.
The final two header bytes carry request/event flags or response status.

Every errno-valued RPC status uses Zephyr/Picolibc numbering on the wire.
The native_sim SDK converts outgoing response statuses with `nsi_errno_to_mid` and received response
and event statuses with `nsi_errno_from_mid`. Pico already uses that numbering.
Codes 1–34 are unchanged; ENOSYS is 88 and ETIMEDOUT is 116.
Firmware keeps host errno numbering internally, so numbers printed in its text can differ between hosts.
