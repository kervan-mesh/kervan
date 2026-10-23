# ksim web UI

Status: **phases 5, 5b, 5c, 6a and 6b.** Edit a scenario on a map, run it in ksim, and replay what happened:
frames, receptions, collisions, relays, the stacks' own events, and every frame's bytes and fields.

```sh
cd web && npm ci && npm run build   # once, and after UI changes (Node.js 20.19+ or 22.12+)
build/sim/ksim serve                # then open http://127.0.0.1:8080
```

`ksim serve [--port P] [--web DIR] [--scenarios DIR] [--hardware DIR] [--allow-processes]
[--virtual-boards N]`

- `--scenarios DIR`: scenarios the UI lists, opens and saves (default: `sim/scenarios` of the
  source tree).
- `--web DIR`: the built UI (default: `web/dist`). Without it the page explains how to build it.
- `--allow-processes`: let UI runs start out-of-process stacks (`"process": {"spawn": ...}`).
  Off by default because a scenario file can name any program. `"wait"` nodes need a terminal and
  only work with `ksim run`.
- `--virtual-boards N`: N boards without hardware on ports `virtual:1`..`virtual:N`
  ([hardware.md](hardware.md#using-boards)).

## How it works

**Run, then replay.** A run takes milliseconds to seconds, so **Run** does not stream: it
sends the scenario to ksim, receives the complete trace, and plays it back like a video (pause,
speed 0.1×–1000×, jump to any moment, step from one radio event to the next). Runs are
deterministic, so the same scenario always gives the same replay and the same trace hash as
`ksim run`. To watch a scenario in real time, use **● Live** (see [Live](#live)).

The UI reads and writes the **same scenario files** as `ksim run`, one line per setting and per
node. Keys it does not know (such as `process`) are kept.

| Area | What it shows |
|---|---|
| Map | Nodes on a meter grid (drag to move, wheel to zoom, drag the background to pan). Links: who can hear whom without interference — solid both ways, dashed one way; green SNR ≥ 5 dB, yellow ≥ −5 dB, orange below. During a replay: rings for frames on air (purple: relayed), green arrows for receptions, dashed red/orange/gray lines for frames lost to collisions, half-duplex or a receiver that was off, and the stacks' events as labels. |
| Scenario tab | Description, simulation settings, path loss model, the radio of nodes without a device, node list. |
| Node tab | Position, device (board), stack, radio overrides and the stack's options — a form generated from the stack's [config schema](simulator.md#config-schema); an empty field means "default" and stays out of the file. After a run: the node's radio statistics, its stack events and end-of-run counters (`*_stats` events). |
| Wiring & firmware (Node tab, Pico + nRF24) | The board and module drawn from the catalog with a wire per signal; choose pins by clicking or in the table; **Build firmware** for those pins (Docker), **Open folder**, **Flash to Pico** (BOOTSEL). See [hardware.md](hardware.md#wiring-and-firmware). |
| Hardware (Node tab) | **Scenario radio**, a **Board**, or **MCU + module** picked from the [hardware catalog](simulator.md#hardware-catalog); shows the parts' key data (RAM, logic voltage, power levels, antenna/LNA, current) and the **wiring**: module pin → MCU pin, with warnings (5 V logic, too little current on the 3.3 V pin) and notes. |
| Real board (Node tab, nRF24 nodes) | The serial port of a real board that runs this node too in live sessions; **Check** asks the board what it is. See [hardware.md](hardware.md). |
| Payload (Node tab) | What the node sends: **Stack default** (the stack's own test data, `len` bytes), **Fixed bytes** (hex, e.g. `01 A2 FF`) or **Python** — a function `payload(node, seq, t)` returning `bytes`, with a seeded `rng` (⤢ opens a larger editor). Python needs `ksim serve --allow-processes`. Format: [simulator.md, Payloads](simulator.md#payloads). |
| Packet tab | Click a `tx`, `rx` or `lost` line in the event list: the frame's sender, airtime, everyone who heard it (RSSI/SNR or why it was lost), its fields as the sending stack decodes them, and a hex dump. Hovering a field highlights its bytes. The map draws the frame's receptions on top. |
| Results tab | Totals, duty cycle per node against the limit, radio losses per node, stack events per node, seed, trace hash and model versions. |
| Timeline | Controls, an activity strip over the whole run (red marks: losses; click to jump) and the events just before the cursor (only the selected node's when one is selected). |

Events are kept on the map for at least 0.15 s of virtual time or 0.25 s of real time, whichever
is longer, so short frames stay visible at high speeds. Slow down (1× or less) to follow single
frames in a busy network.

Keys: Space play/pause, Delete removes the selected node, Esc leaves "Add node".

URL parameters open a given state, e.g. for a link in an issue:
`?scenario=city_mesh&run=1&t=540.9&speed=1&node=1&tab=node&play=0` (`t` in seconds);
`frame=NODE@SECONDS` opens that node's first frame at or after that time in the Packet tab.

## Live

**● Live** runs the scenario in real time instead of all at once: ksim advances virtual time with
the wall clock (10× to start; 1×, 10×, 100× or 1000× in the timeline) and the UI shows events as
they happen. The timeline's **● LIVE** badge means the view follows "now"; scrub back to look at
the past (the replay controls work there) and **Go live** to catch up. **Pause** stops the clock,
**Stop** ends the session early. When it ends, the result is an ordinary run: Results, Packet and
the replay work as after **Run**.

Without real hardware a live run is exactly the run **Run** makes (same trace hash), only paced.
With real boards on some nodes (**● Live + N boards**) the session runs in real time and the
boards' sends and receptions appear in cyan next to the simulation, and Results compares them
([hardware.md](hardware.md)). One live session runs at a time; starting another stops it.
`?live=SPEED` in the URL starts one when the page opens.

How it works: `ksim::Simulator` runs in steps (`start`, `advance_to`, `finish`; `run` is
the three in a row). A `LiveSession` (`sim/src/serve/live.cpp`) owns a simulator and a thread
that advances it to `speed × elapsed wall time` every 20 ms and keeps the trace lines; the UI
reads them as Server-Sent Events.

## API

JSON over HTTP on 127.0.0.1. All POST and PUT bodies are scenario JSON.

| Route | |
|---|---|
| `GET /api/info` | ksim version, ABI, whether scenarios can be saved, `allow_processes` |
| `GET /api/devices` | device profiles: radio defaults and limits |
| `GET /api/stacks` | stacks with version and config schema |
| `GET /api/phys` | radio settings per PHY, in the config-schema format |
| `GET /api/scenarios` | files in the scenario directory: name, description, node count (or an error) |
| `GET /api/scenarios/NAME` | one file, as stored |
| `PUT /api/scenarios/NAME` | save (only valid scenarios; names: letters, digits, `_`, `-`) |
| `POST /api/check` | validate; returns the links (`from`, `to`, `rssi`, `snr` for every decodable direction) |
| `POST /api/run` | run; returns `summary`, `links`, `truncated` and `trace` (the trace lines as an array) |
| `POST /api/wiring` | `{"mcu", "module", "pins": {"CE": "GP15"}}` → the wiring (rows with `mcu_name`, `mcu_number`, `spi`) and `firmware`: `{name, wiring}` or `{error}` |
| `POST /api/firmware/build` | same body: build the firmware for that wiring (needs `--allow-processes`); 409 while one runs |
| `GET /api/firmware/build` | `{state: idle\|running\|done\|failed, name, wiring, file, error, log, seconds}` |
| `GET /api/firmware/drives` | `{"drives": [...]}`: Picos in BOOTSEL mode |
| `POST /api/firmware/flash` | `{"drive"?}`: copy the last built file onto it (`--allow-processes`) |
| `POST /api/firmware/reveal` | open the built file's folder (`--allow-processes`) |
| `GET /api/ports` | `{"ports": [{"port": "COM5"}, ...]}`: serial ports (and virtual boards) |
| `POST /api/ports/hello` | `{"port": "COM5"}` → `{"hello": {...}}`: what the board on that port is ([hardware.md](hardware.md#protocol)) |
| `POST /api/live/start` | `{"scenario": {...}, "speed": 10}`: start a live session (same rules as `/api/run`); returns its status |
| `GET /api/live/stream?from=N` | Server-Sent Events: `data: {state, t_us, duration_us, speed, hardware, total_lines, truncated, from, lines: [...]}` about 5 times a second and whenever lines arrive; ends after `done` or `error` |
| `POST /api/live/control` | `{"action": "pause" \| "resume" \| "stop"}` or `{"action": "speed", "speed": 100}`; with real boards only `stop` (409 otherwise) |
| `GET /api/live/status` | the status without lines |
| `GET /api/live/result` | after `done`: `summary`, `links`, `truncated`, `hardware` (the boards) (409 while running) |
| `POST /api/decode` | `{"stack", "config", "data": hex}` → `{"fields": [{name, value, offset, len}]}` from the stack's `describe_frame`, or `null` |

Errors come back as `{"error": "..."}` with status 400 (bad scenario, rejected config), 403, 404
or 405. Traces longer than 256 MB are cut at a line boundary (`truncated`); the summary always
covers the whole run.

The API is `ksim::serve::Api` (`sim/src/serve/api.cpp`), independent of HTTP so it is tested
without sockets; `http.cpp` adds the server (cpp-httplib).

### Security

The server listens on 127.0.0.1 only and answers only pages it served itself: it rejects foreign
`Host` headers (DNS rebinding), foreign `Origin` headers, and POST/PUT requests that are not
`application/json` (which a web page cannot send to another origin without CORS, which ksim does
not grant). Out-of-process stacks and Python payloads (both run programs on this computer) are off
unless `--allow-processes` is given.

## Developing the UI

```sh
build/sim/ksim serve            # API on :8080
cd web && npm run dev           # UI with hot reload on :5173, /api forwarded to :8080
npm test                        # unit tests (vitest): scenario editing, trace model
npm run typecheck
```

`web/` is TypeScript + React, built with Vite; no other runtime dependencies. Code is split into
`src/model/` (pure functions, unit tested) and `src/components/`. The map is a canvas redrawn on
every change. The UI knows no protocol: stacks describe their options with a config schema and
report events by name, so a new stack, PHY option or board appears without UI changes.

## Showcase

`sim/scenarios/nrf24_payloads.json`: a hub and two sensors, one sending fixed bytes (a text), one a temperature
made by a Python function — open a frame in the Packet tab to see it (needs `--allow-processes`).

`sim/scenarios/city_mesh.json`: 41 sensors on a jittered 450 m grid report to a gateway every
10 minutes over Kervan's managed flooding (SF9, urban path loss, 6 dB shadowing). The replay shows
what flooding costs at this density: about 30 frames per message, thousands of collisions, relays
dropped for lack of duty-cycle budget, and a gateway that has to acknowledge everyone and reaches
its ACK debt limit (`kv_log` events) — the case for route learning and for a gateway with more
airtime.
