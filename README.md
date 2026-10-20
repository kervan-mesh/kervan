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
| 0 | Repository, build system, tests, CI ← *we are here* |
| 1 | Frame format + PAL; two nodes talk over UDP on a PC |
| 2 | Discrete-event simulator with a LoRa channel model |
| 3 | Pub/sub, QoS 1, fragmentation |
| 4 | Multi-hop mesh |
| 5 | Web visualizer |
| 6 | ESP32 + LoRa port, hardware-in-the-loop bridge |
| 7 | More PHYs (BLE, 802.15.4, ESP-NOW), encryption, energy model, MQTT gateway (*Kervansaray*) |

## Building

Requirements: CMake ≥ 3.21 and a C99 compiler (GCC, Clang or MSVC).

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

Apache License 2.0 — see [LICENSE](LICENSE).
