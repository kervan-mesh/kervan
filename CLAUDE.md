# Kervan — guide for Claude

Kervan is a lightweight, transport-agnostic messaging protocol for embedded wireless networks,
plus a simulator that runs the *same* firmware code on virtual nodes over a modeled radio channel,
and a hybrid bridge so real devices can join a simulated network. See README.md for vision and roadmap.

## Architecture (planned)

- `core/` — protocol library, **portable C99**. No dynamic allocation (no `malloc`), no threads,
  no OS calls, no hardware access. Event-driven; all memory sized at compile time. Target: 4–8 KB RAM.
- PAL (Platform Abstraction Layer) — the only way core reaches time, radio, storage, RNG, logging.
  Going "live" on new hardware = writing a new port; core never changes.
- `port/<platform>/` — PAL implementations: `sim` first, then `esp32` (ESP-IDF), `nrf52` (Zephyr),
  `stm32`, `rp2040` (Pico W).
- Simulator — **C++20**, discrete-event engine with virtual time, deterministic (seeded),
  radio channel model (path loss, SNR, collisions, airtime per PHY profile). Nodes loaded in-process.
- Kervansaray — gateway (target: Raspberry Pi Zero 2 W) bridging Kervan ↔ MQTT. C++.
- Web UI — TypeScript + React, talks to `ksim serve` over HTTP (run, then replay the trace).
  WebSocket streaming only when live hardware (phase 6) needs it.
- First PHY profile: **LoRa** (strictest constraints: tiny payloads, long airtime). BLE, 802.15.4,
  ESP-NOW come later.

Exists so far: `core/` (frame, node, PAL), `port/udp/` (PC port, peer lists define topology),
`examples/udp_demo/`, `tests/`, `fuzz/`, `sim/` (ksim simulator), `docs/`.
Wire format: `docs/frame-format.md`. Messaging (pub/sub, QoS, fragments, duty cycle, mesh):
`docs/messaging.md`. Simulator: `docs/simulator.md`. Out-of-process stacks:
`docs/remote-protocol.md` (`sim/remote/`: C client, Python client, `ksim-node`). Web UI: `web/`
(TypeScript + React + Vite) served by `ksim serve` (`sim/src/serve/`, cpp-httplib), see
`docs/web-ui.md`.

Simulator rules: ksim is **general-purpose** — `ksim_core` must never depend on Kervan core; protocols
plug in through the C ABI in `sim/include/ksim/stack.h` (`sim/stacks/<name>/`). Results must be
bit-identical across OSes: no `<random>` distributions, round signal levels before comparing.
Golden tests (`KervanStack.GoldenLineScenario`, `Esb.GoldenStarScenario`) pin trace hashes — update
them only for intended behaviour changes, after checking the trace diff. Unknown scenario keys are
errors. Everything is versioned (PHY `version()`, `ksim_stack_t::version`, device profile `version`,
trace `meta` line): bump the version whenever results can change. Boards are data
(`sim/devices/*.json`); see "Extending ksim" in `docs/simulator.md`. ESB model spec:
`docs/nrf24-esb.md` (clean-room, datasheet values checked).
Stack ABI is v3: `ksim_stack_t::config_schema` (JSON) describes a stack's options for the UI; its
defaults must match the stack's real defaults (`ServeApi.SchemaDefaultsAreTheStackDefaults`).
`ksim serve` lives in its own library (`ksim_serve`), so `ksim_core` needs no network code; the
API (`ksim::serve::Api`) is tested without sockets. The web UI knows no protocol: stacks describe
their options (schema) and report events by name. Keep UI logic in `web/src/model/` (pure, vitest)
and drawing in `web/src/components/`.
Ports drive the node from their main loop (`kv_node_on_rx/on_tx_done/tick`, `next_deadline`) and
never call core from inside a PAL callback or an interrupt.

## Conventions

- Public API prefix `kv_`, types `kv_*_t`, macros `KV_*`. Public headers live in `core/include/kervan/`.
- Every source file starts with:
  ```c
  /*
   * Copyright 2026 The Kervan Authors
   * SPDX-License-Identifier: Apache-2.0
   */
  ```
- Code must compile warning-free with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion`
  (GCC/Clang) and `/W4` (MSVC). CI uses warnings-as-errors.
- Formatting: `.clang-format` (LLVM base, 4 spaces, 100 cols, Linux braces).
- Tests: Unity framework, one `tests/test_<area>.c` per area, registered via `kervan_add_test()`
  in `tests/CMakeLists.txt`. Simulator (C++20) tests use GoogleTest in `sim/tests/`. Core logic is tested with a mock PAL; `test_udp` uses real sockets.
  Wire-format tests use hand-computed golden byte arrays. Untrusted-input parsers get a fuzz target
  in `fuzz/`. Keep core line coverage near 100% (CI fails below 90%).
- Wire format must be explicit about byte order (little-endian) and never rely on struct layout/packing.

## Build & test

Host (any of GCC, Clang, MSVC):
```sh
cmake -S . -B build -DKERVAN_WARNINGS_AS_ERRORS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Dev container (GCC 13, Clang 18, arm-none-eabi 13, Ninja):
```sh
docker build -t kervan-dev .devcontainer
docker run --rm -v "<repo>:/workspaces/kervan" kervan-dev bash -c "<commands>"
```
Put build directories in `/tmp` inside the container to keep the host tree clean, or in a named
volume (`-v kervan-build:/build`, owned by uid 1000) to keep builds between `docker run`s.

Simulator: `build/sim/ksim run sim/scenarios/line_5.json [--seed N] [--trace out.jsonl]`.

Web UI (Node.js 20.19+/22.12+; in the container too):
```sh
cd web && npm ci && npm test && npm run build   # typecheck + vitest + bundle into web/dist
build/sim/ksim serve                            # http://127.0.0.1:8080; `npm run dev` for hot reload
```
Run `npm` for the container on a copy of `web/` (or a separate checkout): `npm ci` there would
replace the host's `node_modules` with Linux binaries. Check UI changes visually with a headless
browser screenshot, e.g. `msedge --headless=new --screenshot=out.png --window-size=1500,950
--virtual-time-budget=10000 "http://127.0.0.1:8080/?scenario=line_5&run=1&t=548&play=0"`.

ARM Cortex-M4 cross build of core (also run by CI — check code size here):
```sh
cmake -S . -B build-arm -DCMAKE_SYSTEM_NAME=Generic -DCMAKE_C_COMPILER=arm-none-eabi-gcc \
  -DCMAKE_C_FLAGS="-mcpu=cortex-m4 -mthumb -Os" -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DKERVAN_BUILD_TESTS=OFF -DKERVAN_WARNINGS_AS_ERRORS=ON
cmake --build build-arm && arm-none-eabi-size -t build-arm/core/libkervan_core.a
```

Sanitizers, coverage and fuzzing (in the dev container; options also listed in CMakeLists.txt):
```sh
CC=clang cmake -S . -B /tmp/b-san -DKERVAN_SANITIZE=ON      # ASan + UBSan, then ctest
CC=gcc   cmake -S . -B /tmp/b-cov -DKERVAN_COVERAGE=ON      # ctest, then: gcovr -r . --filter core/ --filter port/ /tmp/b-cov
CC=clang cmake -S . -B /tmp/b-fz -DKERVAN_BUILD_FUZZERS=ON -DKERVAN_BUILD_TESTS=OFF
/tmp/b-fz/fuzz/fuzz_frame -max_total_time=60                # also fuzz_node
```

CI (`.github/workflows/ci.yml`): GCC, Clang (Linux), MSVC (Windows), Apple Clang (macOS),
arm-none-eabi, ASan/UBSan, coverage (≥90% lines, core + port + sim), 60 s fuzzing per target, web UI
(typecheck, vitest, build) — on every push/PR.

## Roadmap status

- [x] Phase 0 — repo, CMake, Unity tests, CI, dev container
- [x] Phase 1 — frame format + PAL; two nodes talk over UDP on a PC
- [x] Phase 2 — discrete-event simulator (ksim), LoRa channel model
  - [x] 2a — engine, LoRa channel, C ABI stack adapter, Kervan stack, scenario files, `ksim` CLI
  - [x] 2b — PHY abstraction, nRF24 PHY + Enhanced ShockBurst stack, device profiles, ABI v2
    (radio control), versioned models
  - [x] 2c — out-of-process stacks: remote protocol v1, C + Python clients, ksim-node, spawn/wait
- [x] Phase 3 — pub/sub (hashed 16-bit topics), QoS 0/1 with backoff, fragmentation, duty-cycle budget
  (docs/messaging.md)
- [x] Phase 4 — multi-hop mesh: managed flooding (SNR-weighted delay, suppression); route learning later
- [x] Phase 5 — web UI (first version): `ksim serve` + scenario editor (map, devices, stacks with
  schema-generated forms, radio, traffic) + run-then-replay viewer (packet animation, losses by
  reason, duty cycle, stack events), `city_mesh` showcase (docs/web-ui.md). Later: real map tiles
  (OSM), comparing two runs, live streaming for HIL.
- [ ] Phase 6 — ESP32 + LoRa port, hardware-in-the-loop bridge ← next
- [ ] Phase 7 — more PHYs, encryption (AES-CCM), energy model, MQTT gateway

Update this checklist when a phase completes. Record significant design decisions in `docs/`.
