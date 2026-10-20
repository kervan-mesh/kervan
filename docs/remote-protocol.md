# ksim remote protocol (version 1)

Lets a protocol stack run in its own process — any language, its own globals, its own license — and
still take part in a deterministic simulation. The protocol mirrors the C stack interface
(`sim/include/ksim/stack.h`) one to one, so a stack produces **the same trace** whether it runs inside
ksim or out of process. CI checks exactly that.

## Connection

- TCP on `127.0.0.1` only. ksim listens; stack processes connect.
- One connection per node. The process learns where to connect from the environment:
  `KSIM_PORT` (ksim's port) and `KSIM_NODE_ID` (which node it is).
- A scenario node chooses how its process starts:
  - `"process": {"spawn": ["ksim-node", "--stack", "kervan"]}` — ksim starts it. A bare program
    name is looked up next to the `ksim` executable first, then on `PATH`.
  - `"process": {"wait": true}` — ksim prints the port and waits for you to start it, e.g. under a
    debugger. Virtual time stands still while you step through the code.
  - `"timeout_s"`: how long ksim waits for each answer (and for a spawned process to connect).
    Default 10 s for `spawn`, unlimited for `wait`. When it expires, or the process dies, the run
    stops with an error naming the node.

## Lockstep

ksim sends one **event**. While handling it the stack may send **requests**; requests that return a
value are answered immediately with `RESULT`. The stack ends the event with `DONE`. Virtual time
does not advance in between, exactly like a function call in process.

```
ksim                                    stack process
 │── EV_RX (now, rssi, snr, bytes) ───▶│
 │◀── REQ_RADIO_SEND (bytes) ──────────│
 │── RESULT (status) ─────────────────▶│
 │◀── REQ_TIMER_SET (id, at) ──────────│   no answer: void calls are one-way
 │◀── DONE ────────────────────────────│
```

Rules for stacks (needed for reproducible runs):

- Use the `now` of the current event as the time; never read the wall clock.
- Use `REQ_RANDOM` for random numbers; never seed a generator from time or the OS.
- Only send requests while handling an event.

## Framing

Every message: `u32 length` (little-endian, bytes that follow, at most 65536) `| u8 type | fields`.
Integers are little-endian; `f64` is an IEEE 754 double in little-endian byte order; `str` and
`bytes` are `u16 length` + data.

## Messages

| Type | Name | Direction | Fields |
|---|---|---|---|
| 0x01 | HELLO | stack → ksim | `u16 protocol` (1), `u16 abi` (2), `u32 node_id`, `u8 mode` (0 = lockstep), `str stack_name`, `str stack_version` |
| 0x02 | ERROR | ksim → stack | `str message`; ksim closes the connection afterwards |
| 0x10 | EV_CREATE | ksim → stack | `u64 now`, `u32 node_id`, `u32 mtu`, `str config` |
| 0x11 | EV_RX | ksim → stack | `u64 now`, `f64 rssi_dbm`, `f64 snr_db`, `bytes frame` |
| 0x12 | EV_TX_DONE | ksim → stack | `u64 now` |
| 0x13 | EV_TIMER | ksim → stack | `u64 now`, `u32 timer_id` |
| 0x14 | EV_DESTROY | ksim → stack | `u64 now`; after `DONE` both sides close |
| 0x20 | REQ_RADIO_SEND | stack → ksim | `bytes frame` → RESULT |
| 0x21 | REQ_TIMER_SET | stack → ksim | `u32 timer_id`, `u64 at_us` |
| 0x22 | REQ_TIMER_CANCEL | stack → ksim | `u32 timer_id` |
| 0x23 | REQ_TRACE | stack → ksim | `str event`, `str detail` |
| 0x24 | REQ_RANDOM | stack → ksim | → RESULT (value = random `u32`) |
| 0x25 | REQ_RADIO_GET | stack → ksim | `u32 param` → RESULT (status, value) |
| 0x26 | REQ_RADIO_SET | stack → ksim | `u32 param`, `i64 value` → RESULT (status) |
| 0x2F | DONE | stack → ksim | `u8 ok` (EV_CREATE: 0 rejects the config; otherwise 1) |
| 0x30 | RESULT | ksim → stack | `i32 status` (`KSIM_OK`, `KSIM_ERR_*`), `i64 value` |

Constants are in [`sim/include/ksim/remote_protocol.h`](../sim/include/ksim/remote_protocol.h).

## Handshake

1. The process connects and sends `HELLO`.
2. ksim checks the protocol and ABI versions, that it expects this node id, and that `stack_name`
   matches the node's `"stack"` in the scenario. On a mismatch it sends `ERROR` and stops the run.
3. Once every remote node is connected, ksim sends `EV_CREATE` to each node in scenario order.

The stack name and version from `HELLO` are recorded in the trace's `meta` line, exactly as for an
in-process stack.

## Client libraries

| Language | Where | Use |
|---|---|---|
| C | `sim/remote/client` (`ksim/remote.h`) | `ksim_remote_serve_env(stack)` runs any existing `ksim_stack_t` out of process, unchanged |
| Python | `sim/remote/python/ksim_remote.py` | subclass `Stack`, call `serve(MyStack)` |

`ksim-node --stack <name>` serves the built-in stacks (`kervan`, `nrf24-esb`) over this protocol.

## Later

`mode` reserves room for a real-time mode (phase 6), where real devices take part and virtual time
follows the wall clock.
