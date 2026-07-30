# PURPLE 4

The details for the Protocol for Unified Real-time Physical-Logical Exchange (PURPLE), version 4. This protocol is used for communication between router processes and the simulation process.

## Format
The format of PURPLE follows that of TOML. The header indicates the type of message that is sent. 

## Sending
Messages should strictly follow the accepted style guide when being constructed.

```toml
[header]\n
<name> = <value>\n
<name1> = <value2>\n
```

## Receiving
Receivers should be lenient in parsing receiving requests. Additional or dangling spaces should not invalidate a request. An example of an acceptable request is as follows:

```toml
[header]

id  = 234

device=     "heyy"


```

## Messages
Below is a list of currently accepted messages:

### `[init]`
Used to pass the id of the started module to the simulator.

Required elements:
- `id = <uint>`: The id of the module.

Example:
```toml
[init]
id = 12
```

### `[init.finish]`
Used to indicate to the simulator that a module is ready to receive packets.

Required elements:
- `id = <uint>`: The id of the module.

Example:
```toml
[init.finish]
id = 12
```

### `[update]`
Used to indicate the value of a hardware component. May be sent by the simulator or a module.

Required elements:
- `module = <uint>`: The module that this component belongs to.
- `device = <string>`: The changed device. Currently accepted: `"led", "big belt", "small belt", "servo", "left laser", "right laser"`.
- `value = <num>`: The updated value.

Example:
```toml
[update]
module = 12
device = "led"
value = 2298889
```

### `[update.rfid]`
Used to indicate the value of the RFID chip on a sensor sent from the simulation.

Required elements:
- `module = <uint>`: The module that this component belongs to.
- `uid_length = <uint>`: The length of the UID data.
- `uid = <string>`: The UID of the RFID chip.
- `data_length = <uint>`: The length of the data.
- `data = <string>`: The data contained in the rest of the RFID chip, with each byte being encoded as a hex value.

Example:
```toml
[update.rfid]
module = 12
uid_length = 10
uid = "88EA3D46DD"
data_length = 12
data = "0A306B2A4EEE"
```

### `[update.graph]`
Used to update the graph of a module.

Required elements:
- `module = <uint>`: The module that this component belongs to.
- `size = <uint>`: The amount of modules in this graph.
- `length = <uint>`: The length of the graph data.
- `data = <string>`: The data of the graph. Format: `module:left,middle,right`.

Example:
```toml
[update.graph]
module = 12
count = 2
data_length = 11
data = "1:1,2,3|2:3,2,1"
```

### `[packet]`
Used to send a packet from one module to another module.

Required elements:
- `module = <uint>`: The module that this packet is being sent to.
- `length = <uint>`: The length of the data.
- `data = <string>`: The data.

Example:
```toml
[packet]
module = 12
length = 12
data = "2394823492"
```

<!-- ## `[tub.arrival]`
Used to indicate the arrival of a tub in the system.

Required elements:
- `id = <uint>`: The id of the tub.
- `module = <uint>`: The module that this tub arrives at.
- `destination = <uint>`: The destination module. 
- `safe = <bool>`: Whether the tub is safe.
- `needs_security = <bool>`: Whether the tub should be routed to security.

```toml
[tub.arrival]
id = 3
module = 12
destination = 6
safe = false
needs_security = false
``` -->

<!-- ## `[tub.departure]`
Used to indicate the departure of a tub in the system.

Required elements:
- `id = <uint>`: The id of the tub.
- `module = <uint>`: The module that this tub leaves from.

```toml
[tub.departure]
id = 2
module = 6
``` -->

### `[tub.schedule]`
Used to schedule the arrival of a tub in the system. Sent on startup to every module.

Required elements:
- `id = <uint>`: The id of the tub.
- `module = <uint>`: The module that this tub arrives at.
- `destination = <uint>`: The destination module. 
- `safe = <bool>`: Whether the tub is safe.
- `needs_security = <bool>`: Whether the tub should be routed to security.

```toml
[tub.arrival]
id = 3
module = 12
destination = 6
safe = false
needs_security = false
```

### `[plane.arrival]`
Used to indicate the arrival of a plane in the system. Sent by the simulator.

Required elements:
- `id = <uint>`: The id of the plane.
- `module = <uint>`: The module that this plane arrives at.
- `leaves_in = <uint>`: In how many seconds the plane leaves.

```toml
[plane.arrival]
id = 2
module = 6
leaves_in = 23
```

### `[plane.departure]`
Used to indicate the departure of a plane in the system. Sent by the simulator.

Required elements:
- `id = <uint>`: The id of the plane.
- `module = <uint>`: The module that this plane leaves from.

```toml
[plane.departure]
id = 2
module = 6
```
