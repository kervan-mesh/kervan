# Kervan 🐫

**Lightweight, transport-agnostic messaging for embedded wireless networks — with a built-in simulator so you can build and test without hardware.**

> ⚠️ **Status: early design phase.** Nothing here is usable yet. APIs and the wire format will change.

Messages travel node to node like a caravan along the Silk Road: hopping across a mesh, resting at
*kervansaray* gateways, and eventually reaching the wider world (MQTT, the web, your dashboards).

## Why?

Embedded developers juggle many radios (LoRa, BLE, 802.15.4, ESP-NOW, Wi-Fi) and many protocols
(MQTT, MQTT-SN, CoAP, vendor stacks). Testing a 50-node network usually means owning 50 devices.

Kervan aims to give you:

- **One small C library** — portable C99, no dynamic allocation, runs on bare metal or an RTOS.
- **One API over any radio** — publish/subscribe and request/response, regardless of the transport underneath.
- **A simulator that runs the *same* code** — virtual nodes, a modeled radio channel (range, loss, collisions, airtime), deterministic and repeatable.
- **A hybrid bridge** — plug a real device into a simulated network; move from 100% virtual to 100% real one node at a time.

## Architecture

```
┌──────────────────────────────────────────────┐
│  Application                                  │
├──────────────────────────────────────────────┤
│  core     protocol: pub/sub · req/resp · QoS  │
│           fragmentation · mesh · security     │
├──────────────────────────────────────────────┤
│  PAL      time · radio · storage · rng · log  │
├───────────────┬──────────────┬───────────────┤
│  port/sim     │  port/esp32  │  port/nrf52 … │
└───────────────┴──────────────┴───────────────┘
```

The core never touches hardware directly; everything goes through the Platform Abstraction Layer (PAL).
Going "live" means writing a port — the protocol code does not change.

## Roadmap

| Phase | Goal |
|---|---|
| 0 | Repository, build system, tests, CI ✅ |
| 1 | Frame format + PAL; two nodes talk over UDP on a PC ✅ |
| 2 | Discrete-event simulator: LoRa + nRF24L01+ radios, device profiles, stacks in any language ✅ |
| 3 | Pub/sub, QoS 1, fragmentation, duty-cycle budget ✅ |
| 4 | Multi-hop mesh (managed flooding) ✅ |
| 5 | Web UI: build scenarios visually (boards, protocols, radios, traffic) and watch them run ✅ |
| 5b | Payloads (length, fixed bytes or a Python function per node) and a packet inspector in the UI ✅ |
| 5c | Hardware catalog: pick an MCU and a radio module (or a board), with a wiring guide ✅ |
| 6 | Live mode: real boards (Raspberry Pi Pico + nRF24L01+) next to the simulation, then hardware-in-the-loop; channel model calibrated with real measurements |
| 7 | ESP32 + LoRa port, more PHYs (BLE, 802.15.4, ESP-NOW), encryption, energy model, MQTT gateway (*Kervansaray*) |

## Building

Requirements: CMake ≥ 3.21 and a C99 compiler (GCC, Clang or MSVC).

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### Try it: two nodes over UDP

Each node listens on its own UDP port on `127.0.0.1` and transmits to the `--peer` ports you give it,
so the peer lists decide who hears whom. Frames are limited to 51 bytes, like LoRa at SF12.

```sh
# terminal 1
build/examples/udp_demo/kervan_udp_demo --addr 1 --port 40001 --peer 40002 --dst 2
# terminal 2
build/examples/udp_demo/kervan_udp_demo --addr 2 --port 40002 --peer 40001 --dst 1
```

(With MSVC the binary is under `build/examples/udp_demo/Debug/`.)

### Try it: the simulator

`ksim` runs the same Kervan code on virtual nodes over a modelled LoRa channel (path loss, shadowing,
collisions, airtime) in virtual time — an hour of network traffic takes a fraction of a second, and
the same seed gives the same result on every OS.

```sh
build/sim/ksim run sim/scenarios/line_5.json --trace line_5.jsonl
```

It prints delivery and loss statistics, per-node airtime and duty cycle. See
[docs/simulator.md](docs/simulator.md) for the channel models and the scenario format.

The simulator is protocol-agnostic and modular: radios (LoRa, nRF24L01+), protocol stacks (Kervan,
nRF24 Enhanced ShockBurst) and boards (`ksim devices`) are separate, versioned building blocks.
New boards are plain JSON files; new stacks plug in through a small C interface, or run as a
separate process in any language (C and Python clients included) with exactly the same results.

### Try it: the web UI

Build scenarios on a map — boards, protocols, radios, traffic — run them and watch the replay:
frames on air, receptions, collisions, relays, duty cycle per node. Needs Node.js (20.19+ or 22.12+) once to
build the UI.

```sh
cd web && npm ci && npm run build && cd ..
build/sim/ksim serve        # open http://127.0.0.1:8080
```

Try `city_mesh`: 42 nodes in a city district, and what managed flooding costs there. See
[docs/web-ui.md](docs/web-ui.md).

### Dev container (recommended)

The repository ships a ready-to-use environment in [`.devcontainer/`](.devcontainer/) with GCC, Clang,
Ninja, CMake, the `arm-none-eabi` cross toolchain and Node.js — the same tools CI uses.

- **VS Code:** install the *Dev Containers* extension, open the folder, choose *Reopen in Container*.
- **Plain Docker:**
  ```sh
  docker build -t kervan-dev .devcontainer
  docker run --rm -it -v "$PWD:/workspaces/kervan" kervan-dev
  ```

The container also has what CI uses for sanitizers (`-DKERVAN_SANITIZE=ON`), coverage
(`-DKERVAN_COVERAGE=ON` + `gcovr`) and fuzzing (`-DKERVAN_BUILD_FUZZERS=ON`, Clang).

## License

Apache License 2.0 — see [LICENSE](LICENSE).
