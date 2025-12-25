# PURPLE 2
The details for the Protocol for Unified Real-time Physical-Logical Exchange (PURPLE), version 2.

## Format
The format of PURPLE follows that of TOML. The header indicates the type of message that is sent. Currently accepted:

### `[introduction]`
Used to pass the id of the started module to the receiving server.

Required elements:
- `id = <uint>`: The id of the module.

Example:
```toml
[introduction]
id = 12
```

### `[update]`
Used to indicate the value of a hardware component.

Required elements:
- `module = <uint>`: The module that this component belongs to.
- `device = <string>`: The changed device. Currently accepted: `"led", "big belt", "small belt", "servo"`.
- `value = <num>`: The updated value.

Example:
```toml
[update]
module = 12
device = "led"
value = 2298889
```

## `[tub.arrival]`
Used to indicate the arrival of a tub in the system.

Required elements:
- `id = <uint>`: The id of the tub.
- `module = <uint>`: The module that this tub arrives at.
- `destination = <uint>`: The destination module. 
- `safe = <bool>`: Whether the tub is safe.

```toml
[tub.arrival]
id = 3
module = 12
destination = 6
safe = false
```

## `[tub.departure]`
Used to indicate the departure of a tub in the system.

Required elements:
- `id = <uint>`: The id of the tub.
- `module = <uint>`: The module that this tub leaves from.

```toml
[tub.departure]
id = 2
module = 6
```

## `[plane.arrival]`
Used to indicate the arrival of a plane in the system.

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

## `[plane.departure]`
Used to indicate the departure of a plane in the system.

Required elements:
- `id = <uint>`: The id of the plane.
- `module = <uint>`: The module that this plane leaves from.

```toml
[plane.departure]
id = 2
module = 6
```

## `[shutdown]`
Used to indicate to a module to shut down immediately.

```toml
[shutdown]
```

## `[shutdown.confirm]`
Sent from a module to indicate shutdown was completed.

Required elements:
- `id = <uint>`: The id of the plane.

```toml
[shutdown.confirm]
id = 2
```

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