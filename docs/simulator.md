# ksim — the Kervan simulator

Status: engine, LoRa and nRF24 PHYs, device profiles, Kervan and nRF24 ESB stacks, out-of-process
stacks (any language), CLI, and a web UI (`ksim serve`, [web-ui.md](web-ui.md)).

ksim is a general-purpose discrete-event simulator for low-power wireless networks. Kervan is one
protocol it can run; other stacks plug into the same interface. The simulator itself never depends
on Kervan code. Everything is modular and **versioned**: PHY models, stacks and device profiles each
carry a version that is recorded in every trace.

```
┌──────────────────────────────────────────────────────┐
│  Engine: virtual time (µs) · event queue · RNG        │
├──────────────────────────────────────────────────────┤
│  Shared channel: positions · path loss · shadowing ·  │
│  half-duplex · receiver on/off                        │
├───────────────────────┬──────────────────────────────┤
│  PHY: lora (v1)       │  PHY: nrf24 (v1)      │ …    │
├───────────────────────┴──────────────────────────────┤
│  Device profiles: board = MCU + radio + limits (JSON) │
├──────────────────────────────────────────────────────┤
│  Stack adapter interface (C ABI v3, ksim/stack.h)     │
├─────────────┬──────────────────┬─────────────────────┤
│  kervan v2  │  nrf24-esb v1    │ …                   │
└─────────────┴──────────────────┴─────────────────────┘
```

## Running

```sh
ksim run sim/scenarios/line_5.json [--seed N] [--duration S] [--trace out.jsonl] [--devices DIR]
                                    [--port P]
ksim serve [--port P]        # web UI: edit, run and replay scenarios (web-ui.md)
ksim stacks                  # protocol stacks and their versions
ksim devices [--devices DIR] # device profiles
ksim version
```

The summary shows the model versions in use, radio statistics (frames sent, received, lost and why),
application events reported by the stacks, per-node airtime and duty cycle, and a **trace hash**.
Equal hashes mean identical runs.

## Engine

- Virtual time is `uint64` microseconds: LoRa frames take seconds, nRF24 acknowledgements
  hundreds of microseconds.
- Events at the same time run in the order they were scheduled, so runs are reproducible.
- One scenario seed; every node and every link derives its own random stream from it. Adding a node
  does not change the random numbers of the others.
- **Cross-platform determinism:** the same seed must give the same trace on Windows, Linux and
  macOS. ksim therefore uses its own generator (xoshiro256\*\*) and distributions instead of
  `<random>` (whose distributions differ between standard libraries), and rounds signal levels to
  0.01 dB before comparing them. Golden-hash tests check this on all CI platforms.

## Shared channel rules

| Part | Model |
|---|---|
| Path loss | Log-distance: `PL(d) = PL(d0) + 10·n·log10(d/d0) + Xσ`. `PL(d0)` defaults to free-space loss at the frame's carrier frequency. |
| Shadowing | `Xσ`: normal, fixed per link for the whole run, same in both directions |
| Half-duplex | A node that transmits at any time during a frame does not receive it (`half_duplex`) |
| Receiver on/off | Stacks switch the receiver with `KSIM_RADIO_RX_ENABLED`. A frame is received only if the receiver was on, and not retuned, for all of it. On for only part of it → lost (`rx_off`); off for all of it → not listening, not a loss. |
| PHY types | Radios of different types never interact |
| Duty cycle | Measured and reported per node (total airtime / duration); a warning is shown above `duty_cycle_limit`. Not enforced — that is the protocol's job. |

Frames below a receiver's sensitivity are neither delivered nor traced.

## PHY models

A PHY (`sim/src/phy.cpp`, interface in `ksim/phy.hpp`) decides airtime, carrier, whether a receiver
can demodulate a frame, whether two frames interfere and the capture threshold.

### `lora` v1

| Part | Model |
|---|---|
| Airtime | Semtech AN1200.13 formula (SF7–12, BW 125/250/500 kHz, CR 4/5–4/8, LDRO automatic when a symbol exceeds 16 ms) |
| Noise floor | `−174 + 10·log10(BW) + NF` dBm, NF = 6 dB |
| Reception | `SNR ≥` the SF's demodulation floor (SF7 −7.5 dB … SF12 −20 dB, 2.5 dB steps); receiver on the same frequency, SF and bandwidth |
| Collisions | Only frames on the same frequency **and** SF interfere (different SFs are treated as orthogonal). A frame is lost if an overlapping interferer arrives less than `capture_threshold_db` (default 6 dB) below it. |

Known simplifications: hard SNR threshold instead of a PER curve; capture ignores timing; no
inter-SF interference; duty cycle is not measured over sliding one-hour windows.

### `nrf24` v1

nRF24L01+ at 2.4 GHz: see [nrf24-esb.md](nrf24-esb.md) (channels, data rates, sensitivity, C/I,
occupied bandwidth, frame layout).

## Device profiles

A device profile describes a board — MCU plus radio — with the radio's defaults and hardware
limits. Profiles are JSON files: the ones in [`sim/devices/`](../sim/devices/) are compiled into
ksim, and `--devices DIR` loads more (a profile with the same name replaces the built-in one).
Adding a board needs no code.

```json
{
  "name": "esp32-sx1276",
  "version": 1,
  "description": "ESP32 with a Semtech SX1276 LoRa transceiver on PA_BOOST",
  "mcu": "ESP32",
  "radio": { "type": "lora", "freq_hz": 868100000, "sf": 7, "bw_hz": 125000, "tx_power_dbm": 14 },
  "limits": {
    "tx_power_dbm": { "min": 2, "max": 20 },
    "freq_hz": { "min": 137000000, "max": 1020000000 }
  }
}
```

`limits.tx_power_dbm` is either a range or a list of discrete levels (`[-18, -12, -6, 0]` for the
nRF24L01+). Limits are enforced when the scenario is loaded and when a stack changes the radio at
run time. Bump `version` whenever a change could alter results.

## Stack adapter interface (C ABI v3)

Declared in [`sim/include/ksim/stack.h`](../sim/include/ksim/stack.h). A stack provides
`ksim_stack_t` (name, version, create/destroy an instance per node, `on_rx`, `on_tx_done`,
`on_timer`, an optional config schema). The simulator provides `ksim_host_t` (time, `radio_send`, timers, random numbers,
`trace`, `radio_get`/`radio_set`).

- C, not C++: a stack compiled by another compiler, or living in a DLL, still fits.
- Timers instead of a deadline query: they map to every stack we know of (Kervan's
  `kv_node_next_deadline` is translated by its adapter).
- `radio_get`/`radio_set` with `KSIM_RADIO_*` parameters: PHY type (read-only), receiver on/off,
  TX power, LoRa frequency/SF/bandwidth, nRF24 channel/data rate. Values are checked against the
  PHY and the device profile; changes are refused while transmitting.
- `trace` lets stacks report application events, which tests and the summary count.
- `abi_version` guards incompatible changes: v1 initial, v2 added radio control and stack versions,
  v3 the config schema. Out-of-process stacks may still report ABI 2 (the wire protocol did not
  change).
- Stacks are never called back synchronously from a host function: `on_tx_done` and `on_timer`
  always arrive as separate events.

### Config schema

A stack may describe the options `create()` accepts in `ksim_stack_t::config_schema` (JSON, or
NULL). The web UI ([web-ui.md](web-ui.md)) turns it into a form, so a new stack is editable there
without UI code:

```json
{"phy": ["lora"],
 "options": [
   {"key": "relay", "type": "bool", "default": true, "group": "mesh", "help": "Relay frames for other nodes"},
   {"key": "qos", "type": "enum", "values": [0, 1], "default": 0, "group": "traffic", "help": "..."},
   {"key": "len", "type": "int", "min": 0, "max": 254, "default": 10, "group": "traffic", "help": "..."}
 ]}
```

- `phy`: radios the stack runs on (the UI offers it only for those).
- `type`: `int` (with optional `min`, `max`, `step`), `number`, `bool`, `string`, `enum` (`values`).
- `format: "hex"` (for `int`): shown and stored as `"0x..."` (radio addresses); the stack must
  parse it with base 0 (`strtoull(text, &end, 0)`).
- `default`: what applies when the key is left out; omit it when the default depends on the node
  (e.g. an address derived from the node id). A test (`ServeApi.SchemaDefaultsAreTheStackDefaults`)
  runs every built-in stack with all schema defaults spelled out and requires an identical trace.
- `group` and `help` are for display.

## Out-of-process stacks

A node can run its stack in a separate process — another language, firmware with globals, code
under another license, or a node you step through in a debugger. ksim talks to it over TCP on
127.0.0.1 in lockstep, so the trace is **identical** to running the same stack in process (CI
checks this for the Kervan and nRF24 stacks). Protocol and client libraries:
[remote-protocol.md](remote-protocol.md).

```json
{ "id": 2, "pos": [400, 0], "stack": "kervan",
  "process": { "spawn": ["ksim-node", "--stack", "kervan"] } }

{ "id": 3, "pos": [200, 150], "stack": "py-beacon",
  "process": { "spawn": ["${python}", "${scenario_dir}/../remote/python/examples/beacon.py"] } }

{ "id": 4, "pos": [0, 0], "stack": "kervan", "process": { "wait": true } }
```

- `spawn`: ksim starts the command. A bare program name is looked up next to `ksim` first.
  `${python}` becomes `KSIM_PYTHON` if set, else `python` (Windows) or `python3`;
  `${scenario_dir}` is the directory of the scenario file.
- `wait`: ksim prints the port and waits until you start the process yourself (for example in a
  debugger) with `KSIM_PORT` and `KSIM_NODE_ID` set. Virtual time stands still meanwhile.
- `timeout_s`: per answer (default 10 s for `spawn`, unlimited for `wait`). A process that dies,
  hangs or breaks the protocol stops the run with an error naming the node.
- `--port P` fixes the listening port (default: any free port).

Example: [`sim/scenarios/remote_mixed.json`](../sim/scenarios/remote_mixed.json) — Kervan in and
out of process plus Python beacons on one LoRa channel.

## Scenario file (JSON)

```json
{
  "seed": 42,
  "duration_s": 600,
  "mtu": 51,
  "radio": { "type": "lora", "freq_hz": 868100000, "sf": 9, "bw_hz": 125000, "cr": 1,
             "tx_power_dbm": 14 },
  "path_loss": { "exponent": 2.7, "shadowing_sigma_db": 4 },
  "capture_threshold_db": 6,
  "duty_cycle_limit": 0.01,
  "nodes": [
    { "id": 1, "pos": [0, 0], "device": "esp32-sx1276", "stack": "kervan",
      "radio": { "sf": 9 },
      "config": { "send_to": 2, "send_every_ms": 60000, "qos": 1 } },
    { "id": 2, "pos": [800, 0], "stack": "kervan" }
  ]
}
```

- Unknown keys are rejected, so typos do not silently fall back to defaults; settings of the wrong
  radio type (e.g. `sf` on an nRF24) are rejected too.
- A node's radio starts from its device profile (or the scenario `radio` if it has no device) and
  its own `radio` object overrides individual settings.
- `config` is handed to the stack as `key=value;key=value`. Use strings for hexadecimal values
  (`"addr": "0xE7E7E7E701"`).

### Kervan stack config (`kervan` v2, LoRa radios)

Behaviour: [messaging.md](messaging.md).

| Key | Default | Meaning |
|---|---|---|
| `addr` | node id | Kervan address |
| `net` | 1 | network id |
| `hop_limit` | 3 | |
| `ack_timeout_ms`, `retries` | 5000, 3 | first ACK wait (doubles per retry), retransmissions |
| `relay`, `relay_window_ms` | 1, 1000 | take part in the mesh; relay delay window |
| `duty_permille`, `duty_burst_ms`, `ack_debt_ms` | 10, 0, 500 | airtime budget (10 = 1 %, 0 = off) |
| `topic` | `kervan/data` | topic name the traffic generator publishes to |
| `subscribe` | = `topic` | topic name whose broadcasts the node receives |
| `send_to` | — | destination address (65535 = broadcast) |
| `send_every_ms` | 0 (off) | traffic period; the first message goes out at a random offset |
| `jitter_ms` | 0 | each period varies randomly by up to ±jitter (real clocks drift; strictly periodic senders keep colliding with the same partners) |
| `len` | 10 | payload bytes (up to 254; larger than one frame = fragmented) |
| `qos` | 0 | 1 = acknowledged (needs a unicast `send_to`) |

It reports `app_tx`, `app_tx_busy` (queue full), `app_rx` (with `hops`), `app_ack`,
`app_ack_timeout`, `kv_relay`, `kv_relay_cancel`, `kv_relay_drop`, `kv_budget_wait`, `kv_retx`, and
one `kv_stats` line per node at the end.

### nRF24 ESB stack config (`nrf24-esb` v1, nRF24 radios)

See [nrf24-esb.md](nrf24-esb.md#stack-config).

## Trace format

JSON Lines, one event per line. The first line records the versions of everything that produced
the run:

```json
{"t":0,"ev":"meta","ksim":"0.1.0","abi":3,"phy":{"lora":"1"},"stack":{"kervan":"2"},"device":{"esp32-sx1276":"1"}}
{"t":1520384,"ev":"tx","node":1,"len":18,"airtime":185344}
{"t":1705728,"ev":"rx","node":2,"from":1,"rssi":-101.23,"snr":15.8}
{"t":1705728,"ev":"lost","node":3,"from":1,"reason":"collision"}
{"t":1705728,"ev":"app","node":2,"name":"app_rx","detail":"from=1 seq=7 len=10"}
```

Loss reasons: `collision`, `half_duplex`, `rx_off`.

## Extending ksim

| Add | Where | Code? |
|---|---|---|
| A board | `sim/devices/<name>.json` or a `--devices` directory | no |
| A protocol stack (built in) | `sim/stacks/<name>/`, implement `ksim_stack_t`, register in `sim/src/main.cpp` | C (or anything with a C ABI) |
| Its form in the web UI | `config_schema` in `ksim_stack_t` (see [Config schema](#config-schema)) | JSON in a C string |
| A protocol stack (separate process) | anywhere; C: `ksim_remote_serve_env(stack)`, Python: subclass `ksim_remote.Stack` | any language that speaks [the protocol](remote-protocol.md) |
| A radio technology | subclass `Phy` in `sim/src/phy.cpp`, add its settings to `RadioConfig` and the scenario parser | C++ |

Every addition gets a version, tests, and an entry in these docs.
