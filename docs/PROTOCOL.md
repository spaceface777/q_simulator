# Quercus RPC

This document defines the RPC frame and shared-memory exchange between each Quercus application process and the simulation process. The simulator and applications compile the wire types from the pinned `q_sdk` headers.

## Format

Each RPC frame has an 8-byte packed header followed by zero or more payload bytes. A complete frame is at most 1024 bytes. All multi-byte header fields use the host representation used by the `native_sim` application and simulator.

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | `type` | Request, response, event, tick, or acknowledgement |
| 1 | 1 | `cmd` | RPC command for requests and responses, or event mask for events |
| 2 | 2 | `call_id` | Request and response correlation id |
| 4 | 2 | `payload_len` | Number of payload bytes after the header |
| 6 | 2 | `flags` or `status` | Request or event flags, or response status |

The accepted types are `MT_REQ` (1), `MT_RESP` (2), `MT_EVENT` (3), `MT_TICK` (4), and `MT_ACK` (5). The lockstep shared-memory adapter uses the tick and acknowledgement types. Normal platform calls use requests, responses, and events.

## Sending

The sender encodes one complete header and payload into one mailbox slot. A mailbox has four slots and 4088 payload bytes. A frame must have a valid type, a non-negative payload length, and a total size of at most 1024 bytes.

A request uses a unique `call_id`. A confirmable request receives one response with the same `cmd` and `call_id`. A request with `RPC_FLAG_NON_CON` does not receive a response. An event does not receive a response.

The simulator supplies one inbound and one outbound mailbox for each relative tick in the granted batch. The sender writes only while the other process is blocked at the lockstep barrier.

## Receiving

The receiver validates the mailbox slot offsets before it reads frame bytes. It then validates the header and waits for the complete payload. An invalid mailbox or frame stops the run because the two processes no longer share a valid protocol state.

A response has status 0 for success or a negative errno value for failure. Common values are permission denied (`-1`), not found (`-2`), try again (`-11`), invalid argument (`-22`), and not supported (`-38`). The platform can also return range and no-entry errors for commands that need them.

Requests cross an application handoff before the platform handles them. Each module has its own three deterministic Core0 handler lanes and per-category mutexes. A confirmable request occupies its lane through the handoff of its response to that module, including while it waits for another request from the same module and command category; different categories on one module and the same category on different modules may run together. Non-confirmable work releases its lane when its handler returns. Seeded same-tick intake order determines lane assignment. Responses and events return on an executed local tick. A skipped module tick also pauses that module's RPC service delays and rate-limit windows. Laser events have a configured additional delay. RFID event delays use a configured seeded distribution. Network packets, logs, and panic reports pass through the configured network delay and loss model.

## Messages

The command values are defined in `quercus/rpc_wire.h`. Unless a command says otherwise, numeric
payloads use the native byte order of the application and simulator. Payload lengths below are
exact: an invalid length returns `-EINVAL` and does not perform the command. The main command
groups are below.

### Laser sensors

`CMD_LASER_LEFT_DETECT` and `CMD_LASER_RIGHT_DETECT` take no payload and return exactly one
`uint8_t`. A value of `0` means false and `1` means true; other boolean values are never emitted.
`CMD_LASER_LEFT_GET` and `CMD_LASER_RIGHT_GET` also take no payload and return whether the sensor
is enabled. The matching `SET` commands take exactly one `uint8_t`, which must be `0` or `1`.

The platform can publish `EVENT_LASER_LEFT_DETECT` and `EVENT_LASER_RIGHT_DETECT` when an enabled subscribed beam changes from clear to blocked.

### Belts and encoders

`CMD_BELT_BIG_GET_SPEED` and `CMD_BELT_SMALL_GET_SPEED` take no payload and return a native-order
`int32_t` whole-number speed percentage. The matching `SET_SPEED` commands take exactly one
native-order `int32_t`. A valid command is `0` or a magnitude from `25` through `100` percent;
fractional values are not part of the wire contract.

`GET_ENCODER_COUNT` takes no payload and returns a native-order `int32_t` count. `GET_ENCODER_FREQ`
takes no payload and returns a native-order `uint32_t` count rate in milli-Hz. The physical
calibration determines both values.

### Servo and LED

`CMD_SERVO_ANGLE_GET` takes no payload and returns a native-order `int32_t` angle in degrees.
`CMD_SERVO_ANGLE_SET` takes exactly one native-order `int32_t` from `0` through `180` degrees.

`CMD_LED_GET_COLOR` takes no payload and returns a native-order `uint32_t` in `0x00RRGGBB`
format. `CMD_LED_SET_COLOR` takes exactly the same four-byte value; the high byte is reserved and
is clear in the stored and returned color.

### RFID reader

`CMD_RFID_CHECK_TAG` takes no payload and returns exactly one `uint8_t`, `0` or `1`. A value of `1`
means that a tag is in the reader. `CMD_RFID_GET_UID` takes no payload and returns the seven-byte
UID, or an empty payload when no tag is present.

`CMD_RFID_READ_DATA_BLOCK` takes one page index and returns its four bytes. `CMD_RFID_WRITE_DATA_BLOCK` takes one page index followed by four data bytes. Valid payload pages are 0 through 31.

The platform can publish `EVENT_RFID_DETECT` with the seven-byte UID when a subscribed tag enters the reader.

### Current and voltage sensor

`CMD_CV_SENSOR_GET_CURRENT`, `CMD_CV_SENSOR_GET_POWER`, and `CMD_CV_SENSOR_GET_VOLTAGE` take no
payload and return one native-order `float`. The world model calculates the result from the module
calibration and current actuator state.

### Module network

`CMD_SEND_PACKET` takes a destination module byte followed by packet data. A configured module ID is accepted even when that module is offline; its packet traverses the modeled network leg and is discarded because there is no active runtime to receive it. An ID absent from the layout returns not found. `CMD_BROADCAST_PACKET` takes packet data and sends it to every other active module. The configured network model applies one delay and loss result to each delivery leg.

A successful send means the local UDP-like interface accepted the packet, not that the destination received it. Applications must tolerate packet loss, late delivery, and reordering.

`CMD_GET_OWN_ID`, `CMD_GET_NETWORK_MAP`, and `CMD_GET_MODULE_LIST` take no payload. The first
returns one module-id byte, the second returns the layout map, and the third returns a 16-bit module
count followed by five bytes for each active module.

The platform publishes `EVENT_MESSAGE_RECEIVED` with packet data when a subscribed delivery reaches its destination.

### Flight schedule

`CMD_SET_FLIGHT_SCHEDULE` accepts a packed `quercus_flight_board_t`. Only module 255 can set the schedule. The platform returns permission denied for other modules and invalid argument for a malformed board.

### Event subscriptions

`CMD_SUBSCRIBE_TO_EVENT` and `CMD_UNSUBSCRIBE_FROM_EVENT` take exactly one native-order `uint32_t`
event mask. A module receives only events in its current mask. Module 255 cannot subscribe to
physical hardware events.

The supported masks are left laser, right laser, RFID, long encoder stopped, short encoder stopped, current threshold, power threshold, and message received.

### Application lifecycle and diagnostics

`CMD_HEARTBEAT` takes no payload, marks an application as active, and keeps its lifecycle check
current. The simulator reports a late heartbeat, then safe-stops and resets only that application's
state after the failure limit. Its lifecycle remains `STOPPED` for monitoring while other
applications and global simulation time continue.

`CMD_LOG` and `CMD_PANIC_REPORT` carry application diagnostic bytes. They use the normal network model plus the configured log-forwarding delay.
