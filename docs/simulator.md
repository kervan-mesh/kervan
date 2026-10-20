# ksim — the Kervan simulator

Status: **phase 2a** — engine, LoRa channel model, Kervan adapter, CLI.

ksim is a general-purpose discrete-event simulator for low-power wireless networks. Kervan is one
protocol it can run; other stacks (nRF24L01 Enhanced ShockBurst, LoRaWAN, ...) plug into the same
interface. The simulator itself never depends on Kervan code.

```
┌──────────────────────────────────────────────┐
│  Engine: virtual time (µs) · event queue · RNG │
├──────────────────────────────────────────────┤
│  Channel / PHY models: LoRa (first) …         │
│  bytes + airtime + signal strength only       │
├──────────────────────────────────────────────┤
│  Stack adapter interface (C ABI, ksim/stack.h)│
├───────────┬───────────┬──────────────────────┤
│  kervan   │ nrf24-esb │ …                    │
└───────────┴───────────┴──────────────────────┘
```

## Running

```sh
ksim run sim/scenarios/line_5.json [--seed N] [--duration S] [--trace out.jsonl]
ksim stacks        # list built-in stacks
```

The summary shows radio statistics (frames sent, received, lost and why), application events
reported by the stacks, per-node airtime and duty cycle, and a **trace hash**. Equal hashes mean
identical runs.

## Engine

- Virtual time is `uint64` microseconds: LoRa frames take seconds, nRF24 acknowledgements
  hundreds of microseconds.
- Events at the same time run in the order they were scheduled, so runs are reproducible.
- One scenario seed; every node and every link derives its own random stream from it. Adding a node
  does not change the random numbers of the others.
- **Cross-platform determinism:** the same seed must give the same trace on Windows, Linux and
  macOS. ksim therefore uses its own generator (xoshiro256\*\*) and distributions instead of
  `<random>` (whose distributions differ between standard libraries), and rounds signal levels to
  0.01 dB before comparing them. A golden-hash test checks this on all CI platforms.

## LoRa channel model

| Part | Model |
|---|---|
| Airtime | Semtech AN1200.13 formula (SF7–12, BW 125/250/500 kHz, CR 4/5–4/8, LDRO automatic when a symbol exceeds 16 ms) |
| Path loss | Log-distance: `PL(d) = PL(d0) + 10·n·log10(d/d0) + Xσ`. `PL(d0)` defaults to free-space loss at the carrier frequency. |
| Shadowing | `Xσ`: normal, fixed per link for the whole run, same in both directions |
| Noise floor | `−174 + 10·log10(BW) + NF` dBm, NF = 6 dB |
| Reception | Frame is received if `SNR ≥` the SF's demodulation floor (SF7 −7.5 dB … SF12 −20 dB, 2.5 dB steps) and the receiver listens on the same frequency, SF and bandwidth |
| Collisions | Only frames on the same frequency **and** SF interfere (different SFs are treated as orthogonal). A frame is lost if an overlapping interferer arrives less than `capture_threshold_db` (default 6 dB) below it. |
| Half-duplex | A node that transmits at any time during a frame does not receive it |
| Duty cycle | Measured and reported per node (total airtime / duration); a warning is shown above `duty_cycle_limit`. Not enforced — that is the protocol's job. |

Known simplifications, to be refined when measurements call for it: hard SNR threshold instead of a
PER curve; capture ignores timing (a later, stronger frame can capture the receiver at any point);
no inter-SF interference; duty cycle is not measured over sliding one-hour windows.

## Stack adapter interface (C ABI)

Declared in [`sim/include/ksim/stack.h`](../sim/include/ksim/stack.h). A stack provides
`ksim_stack_t` (create/destroy an instance per node, `on_rx`, `on_tx_done`, `on_timer`). The
simulator provides `ksim_host_t` (time, `radio_send`, timers, random numbers, `trace`).

- C, not C++: a stack compiled by another compiler, or living in a DLL, still fits.
- Timers instead of a deadline query: they map to every stack we know of (Kervan's
  `kv_node_next_deadline` is translated by its adapter).
- `trace` lets stacks report application events ("message delivered"), which tests and the summary
  count.
- `abi_version` guards future changes (radio reconfiguration comes with phase 2b).
- Stacks are never called back synchronously from a host function: `on_tx_done` and `on_timer`
  always arrive as separate events.

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
    { "id": 1, "pos": [0, 0], "stack": "kervan",
      "config": { "send_to": 2, "send_every_ms": 60000, "ack": true } },
    { "id": 2, "pos": [800, 0], "stack": "kervan", "radio": { "sf": 9 } }
  ]
}
```

Unknown keys are rejected, so typos do not silently fall back to defaults. A node's `radio` object
overrides the scenario default. `config` is handed to the stack as `key=value;key=value`.

### Kervan stack config

| Key | Default | Meaning |
|---|---|---|
| `addr` | node id | Kervan address |
| `net` | 1 | network id |
| `hop_limit` | 3 | |
| `ack_timeout_ms`, `retries` | 5000, 3 | |
| `send_to` | — | destination address (65535 = broadcast) |
| `send_every_ms` | 0 (off) | traffic period; the first message goes out at a random offset |
| `jitter_ms` | 0 | each period varies randomly by up to ±jitter (real clocks drift; strictly periodic senders keep colliding with the same partners) |
| `len` | 10 | payload bytes |
| `ack` | false | request acknowledgements |

It reports `app_tx`, `app_tx_busy`, `app_rx`, `app_ack`, `app_ack_timeout`.

## Trace format

JSON Lines, one event per line, e.g.

```json
{"t":1520384,"ev":"tx","node":1,"len":18,"airtime":185344}
{"t":1705728,"ev":"rx","node":2,"from":1,"rssi":-101.23,"snr":15.8}
{"t":1705728,"ev":"lost","node":3,"from":1,"reason":"collision"}
{"t":1705728,"ev":"app","node":2,"name":"app_rx","detail":"from=1 seq=7 len=10"}
```

Frames below a receiver's sensitivity are not traced (the receiver never notices them).
