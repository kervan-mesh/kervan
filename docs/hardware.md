# Real boards in live sessions

Status: **phase 6b.** A scenario node can also run on a real board: a Raspberry Pi Pico (or Pico
W) with an nRF24L01+ module. During a **● Live** session the board does what the simulated node
does — same channel, data rate, power step, addresses, retries, payload — and reports every send
and reception over USB. ksim puts these reports into the trace next to the simulation, so you
watch the real radio and the model side by side and compare them afterwards.

Phase 6c will let boards exchange their frames through ksim (real and virtual nodes in one
network); phase 6d will use measurements like these to calibrate the channel model.

## What you need

- Raspberry Pi Pico or Pico W, with a USB cable
- nRF24L01+ module: the small one with a PCB antenna, or the PA/LNA one with an SMA antenna
- 7 jumper wires, and a 10–100 µF capacitor across the module's VCC and GND (strongly advised,
  required for PA/LNA modules)

## Wiring

The default wiring, the same for every nRF24 module (the UI draws it under Node tab → Hardware →
Wiring & firmware; `ksim wiring rpi-pico nrf24l01p` prints it). Other pins work too: see
[Wiring and firmware](#wiring-and-firmware).

| nRF24 module | Pico pin |
|---|---|
| VCC | 3V3(OUT), pin 36 — **never 5 V** |
| GND | GND, pin 38 |
| CE | GP20, pin 26 |
| CSN | GP17, pin 22 |
| SCK | GP18, pin 24 |
| MOSI | GP19, pin 25 |
| MISO | GP16, pin 21 |
| IRQ | not needed (GP21, pin 27, if you wire it) |

A PA/LNA module draws over 100 mA while sending: give it the capacitor or its own 3.3 V
regulator. Two PA/LNA modules closer than about a metre can overload each other's receiver; use
a low power step (`tx_power_dbm` 2 in the scenario) on the bench.

## Wiring and firmware

In the Node tab, section Hardware, a node with **MCU + module** (Raspberry Pi Pico or Pico W with
an nRF24 module) gets **Wiring & firmware**: the board's pin header and the module's header as seen
from above, with a coloured wire per signal (⤢ opens a larger view).

- **Choose pins** by clicking a module pin and then a pin of the board, or in the table. Only
  pins that can work are offered: CE, CSN and IRQ go to any GPIO; SCK, MOSI and MISO must be on
  one SPI controller (Pico: SPI0 on GP0–7 and GP16–22, SPI1 on GP8–15 and GP26–28) and follow
  each other when you move one to the other controller; a pin that is taken swaps places. VCC and
  GND stay on 3V3(OUT) and GND. The scenario keeps only what differs from the default:
  `"pins": {"CE": "GP15"}` on the node (no effect on the simulation). **Default pins** goes back.
- **Build firmware** builds `firmware/pico_nrf24` for exactly these pins, in the dev container
  (Docker Desktop must run and the `kervan-dev` image exist). The first build takes a minute, later
  ones seconds. The file lands in `build/uf2/`, named after the board and pins, e.g.
  `kervan_nrf24-rpi-pico-spi0-sck18-mosi19-miso16-csn17-ce15.uf2`.
- **Open folder** shows it in the file manager. **Flash to Pico** appears while a Pico is plugged
  in with BOOTSEL held (its `RPI-RP2` drive is found) and copies the file onto it.
- Building and flashing run programs on your computer, so `ksim serve` offers them only with
  `--allow-processes`.

A board reports the pins it was built for (`wiring` in `hello`); **Check** under Real board shows
them and warns when the node is wired differently.

From the command line:

```sh
ksim wiring rpi-pico nrf24l01p CE=GP15                 # the wiring with your pins
ksim firmware rpi-pico nrf24l01p CE=GP15               # build it in Docker -> build/uf2/...
ksim firmware rpi-pico-w nrf24l01p-pa-lna SCK=GP10 MOSI=GP11 MISO=GP12
```

## Firmware by hand

The firmware is `firmware/pico_nrf24` (C, Pico SDK 2.2). Build it in the dev container, which has
the SDK and picotool:

```sh
docker run --rm -v "<repo>:/workspaces/kervan" kervan-dev bash -c \
  "cmake -S firmware/pico_nrf24 -B build-pico -G Ninja && cmake --build build-pico"
# Pico W: add -DPICO_BOARD=pico_w (and use another build directory)
# other pins: -DKFW_SPI=1 -DKFW_PIN_SCK=10 -DKFW_PIN_MOSI=11 -DKFW_PIN_MISO=12 -DKFW_PIN_CSN=13 -DKFW_PIN_CE=14
```

The result is `build-pico/kervan_nrf24.uf2`. CI builds both variants on every push (artifact
`firmware-uf2`). Outside the container the same commands work with `arm-none-eabi-gcc`; the build
then downloads the SDK by itself (or uses `PICO_SDK_PATH`).

To flash it: hold the **BOOTSEL** button while plugging the Pico in, a drive `RPI-RP2` appears,
copy the `.uf2` onto it. The Pico restarts and blinks its LED once. It blinks again for every
frame it sends or receives during a session.

Check that the board answers:

```sh
ksim ports              # e.g. COM5 (Windows), /dev/ttyACM0 (Linux), /dev/cu.usbmodem1101 (macOS)
ksim hello COM5
{"ev":"hello","fw":"kervan-nrf24","version":2,"proto":1,"board":"rpi-pico","id":"E6605838...","radio":"nrf24l01p","wiring":"SPI0 SCK=GP18 MOSI=GP19 MISO=GP16 CSN=GP17 CE=GP20"}
```

`"radio":"none"` means the Pico runs but cannot talk to the module: check the wiring and the
supply. On Linux, add yourself to the `dialout` group to open serial ports.

## Using boards

1. Open a scenario whose nodes use the `nrf24-esb` stack on an nRF24 radio, e.g. `pico_nrf24`.
2. In the Node tab, section **Real board**, pick the board's serial port (↻ looks again,
   **Check** asks the board what it is). The file gets `"hardware": {"port": "COM5"}` on that node.
3. **● Live + N boards** starts the session: ksim configures every board like its node, starts
   them together with the simulation and runs in **real time** (no pause, no other speed).

What you see:

- a small cyan square on nodes with a board;
- cyan dashed rings for the boards' sends, cyan arrows for their receptions (next to the
  simulation's own rings and arrows), labels such as `real ✓ (1 retr.)` or `real ✗ no ACK`;
- event lines like `real tx 12 B: ACK, 1 retransmission, received by 1`;
- after the session, Results starts with **Real boards vs. simulation**: frames sent,
  acknowledged, retransmissions per frame and frames received, for the board and for the
  simulated node.

Board reports reach ksim a little late (USB, and ksim waits 150 ms to sort the reports of all
boards), so while following "now" the view runs 0.4 s behind real time.

Without boards: `ksim serve --virtual-boards 2` adds ports `virtual:1` and `virtual:2`. They run the
firmware's own code on modelled nRF24 chips that always hear each other — handy to try the
workflow, not a measurement.

### What a board runs

The board runs the node's `nrf24-esb` options (`addr`, `aw`, `pipes`, `listen`, `send_to`,
`send_every_ms`, `jitter_ms`, `len`, `no_ack`, `ard_us`, `arc`, `ack_payload_len`) and radio
settings (channel, data rate, CRC). It uses the chip's own Enhanced ShockBurst (auto-ACK,
retransmission, dynamic payloads, ACK payloads), as most nRF24 projects do. The power step is the
node's `tx_power_dbm` among the module's four levels (a PA module amplifies the chip's
−18/−12/−6/0 dBm). Payloads: the stack's test data (byte 0 = node id, then counter + i) or the
node's fixed bytes. Not yet: Python payloads, other stacks, other boards.

Limits of the comparison: a board's times count from its start and are exact to about a
millisecond (USB); the trace hash covers the simulation only; `ksim run` ignores `"hardware"`.

## Protocol

JSON objects, one per line, over the USB serial port (the baud rate does not matter).
Protocol version 1 (`proto` in `hello`); ksim refuses boards with another version.

ksim → board:

| Line | |
|---|---|
| `{"cmd":"hello"}` | answer with `hello` |
| `{"cmd":"config", ...}` | settings, see below; answer `ok` or `error` |
| `{"cmd":"start"}` | start sending/listening; event times count from here; answer `ok` |
| `{"cmd":"stop"}` | stop; answer `ok` |
| `{"cmd":"ping"}` | answer `{"ev":"pong","t":...}` |

`config` keys (integers unless noted; defaults are the `nrf24-esb` stack's): `node`, `channel`
(0–125), `rate` (250000, 1000000, 2000000), `crc` (1, 2), `pa` (0–3: −18, −12, −6, 0 dBm at the
chip), `aw` (3–5), `addr`, `pipes` (1–5), `listen` (bool), `send_to`, `every_ms`, `jitter_ms`,
`len` (0–32), `payload` (hex string, up to 32 bytes), `no_ack` (bool), `ard_us` (250–4000, steps
of 250), `arc` (0–15), `ack_payload_len` (0–32). Unknown keys are errors.

Board → ksim:

| Line | |
|---|---|
| `{"ev":"hello","fw":"kervan-nrf24","version":2,"proto":1,"board":"rpi-pico","id":"...","radio":"nrf24l01p","wiring":"SPI0 SCK=GP18 ..."}` | `radio`: `"none"` if no module answers; `wiring`: the pins it was built for (firmware version 2 on) |
| `{"ev":"ok","cmd":"config"}` | |
| `{"ev":"error","msg":"..."}` | a refused command, or trouble while running |
| `{"ev":"tx","t":1234,"seq":0,"data":"4D65","ack":true,"retries":1,"ack_data":"A0A1"}` | a send ended; `t` = when it started (µs); `no_ack: true` when no ACK was asked |
| `{"ev":"rx","t":1500,"pipe":1,"data":"4D65","rpd":true}` | a reception; `rpd`: received power above −64 dBm |
| `{"ev":"tx_busy","t":...}` | a send was due while the last one was still going |

In the trace a report becomes `{"t":..., "ev":"hw", "node":N, "kind":"tx", ...}` with `t` on the
simulation's clock; `kind` `"hello"` lines (time 0) name each node's port and board.

## Code

| Path | |
|---|---|
| `firmware/common/` | portable C99: nRF24 driver from the datasheet (`nrf24.c`, own code: RF24 is GPL), protocol JSON (`json.c`), the firmware's logic (`app.c`) |
| `firmware/testing/` | register-level model of the nRF24L01+ for tests (also the virtual boards) |
| `firmware/pico_nrf24/` | the Pico SDK project: pins, USB, LED |
| `sim/src/hw/` | ksim's side: serial ports, the protocol, virtual boards, firmware builds (`firmware.cpp`) |
| `web/src/components/WiringDesigner.tsx` | the drawing (SVG from the catalog's headers), pin choice; logic in `web/src/model/wiring.ts` |
| `sim/src/serve/live.cpp` | boards in live sessions |

Tests: `tests/test_fw_*.c` (driver, JSON, firmware logic, Unity), `sim/tests/test_hardware.cpp`
(live sessions with virtual boards, API), fuzz target `fuzz_fw_command`.

## Troubleshooting

| Problem | Try |
|---|---|
| `no answer to hello ... is the Kervan firmware on the board?` | flash the `.uf2`; close serial monitors that hold the port |
| `in use by another program` | another program (serial monitor, a second ksim) has the port |
| `the board found no nRF24 radio` | wiring (MISO/MOSI swapped?), 3.3 V supply, capacitor |
| every send `no ACK` | same channel and data rate on both? `send_to` = the receiver's `addr`? receiver `listen` on? PA/LNA modules too close or without capacitor |
| `the radio did not finish sending` | CE not connected (GP20), or the module browns out while sending |
| `Docker is not running` (Build firmware) | start Docker Desktop; the first time also `docker build -t kervan-dev .devcontainer` |
| the board's firmware pins differ from the node's | build and flash for the node's pins, or wire the module as the board expects |
