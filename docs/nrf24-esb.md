# nRF24L01+ / Enhanced ShockBurst model

Status: **model version 1** (`nrf24` PHY v1, `nrf24-esb` stack v1).

A clean-room model written from the behaviour described in the Nordic nRF24L01+ product
specification (v1.0). No vendor or library code is used. Values marked *(verify)* should be
re-checked against the datasheet; everything else is a direct consequence of the packet format.

## PHY (`"type": "nrf24"`)

| Item | Model |
|---|---|
| Channel | 0–125, carrier = 2400 + channel MHz |
| Data rate | 250 kbps, 1 Mbps, 2 Mbps (GFSK) |
| TX power | 0, −6, −12, −18 dBm (limits come from the device profile) |
| Sensitivity | −94 dBm @ 250 kbps, −85 dBm @ 1 Mbps, −82 dBm @ 2 Mbps; a frame is received if RSSI ≥ sensitivity |
| Co-channel capture (C/I) | 12 dB @ 250 kbps, 9 dB @ 1 Mbps, 7 dB @ 2 Mbps *(verify)* |
| Occupied bandwidth | 1 MHz at 250 kbps / 1 Mbps, 2 MHz at 2 Mbps: a 2 Mbps frame also hits the neighbouring channels |
| Interference | Frames interfere when their occupied bands overlap, regardless of data rate |

Simplifications: no adjacent-channel selectivity curve (bands either overlap or not); hard
sensitivity threshold; preamble is one byte at every rate.

### Frame on air

```
preamble 1 B | address 3–5 B | packet control field 9 bit | payload 0–32 B | CRC 1–2 B
```

Packet control field (PCF): payload length (6 bit), PID (2 bit), NO_ACK (1 bit).

Airtime = (8 + 8·address + 9 + 8·payload + 8·CRC) bits / data rate. Examples (5-byte address, 2-byte
CRC): empty ACK 73 bits = 73 µs @ 1 Mbps, 292 µs @ 250 kbps; 32-byte payload 329 µs @ 1 Mbps.

In the simulator `radio_send` takes `address | PCF (2 bytes: len<<2 | PID, NO_ACK) | payload`; the
PHY counts the two PCF bytes as 9 bits on air and adds preamble and CRC itself.

## ESB stack (`"stack": "nrf24-esb"`)

Models the chip's link layer the way firmware sees it (PRIM_RX, EN_AA, ARD/ARC, dynamic payload).

### Timing

| Item | Value |
|---|---|
| Settling (standby/RX → TX, TX → RX, RX → TX) | 130 µs |
| Auto-acknowledge | the receiver (PRX) starts the ACK 130 µs after the frame ends |
| ARD (auto retransmit delay) | 250–4000 µs in 250 µs steps, **from the end of a transmission to the start of the next** |
| ARC (auto retransmit count) | 0–15 retransmissions |

### Transmitter (PTX)

1. A new payload gets the next PID (2-bit counter); retransmissions keep it.
2. Leave RX, settle 130 µs, transmit.
3. With NO_ACK: done (TX_DS) when the frame has been sent.
4. Otherwise listen for the ACK from 130 µs after the frame ends. An ACK is any frame on the
   destination address that arrives completely before the ARD expires *(verify: whether the chip
   can mistake another transmitter's data frame to the same address for its ACK)*.
   - ACK received → TX_DS (and the ACK payload, if any). Done.
   - ARD expires → retransmit, up to ARC times; then MAX_RT. Done.
5. Back to listening (PRX) if the node listens, else standby.

Consequence to verify in tests: the PTX only waits ARD. At 250 kbps an empty ACK ends 130 + 292 =
422 µs after the frame, so **ARD must be ≥ 500 µs at 250 kbps** — the datasheet's rule. At 1 Mbps an
ACK with up to 5 payload bytes fits ARD = 250 µs, 6 bytes do not.

### Receiver (PRX)

- Pipes 1–5: pipe 1 has the full base address, pipes 2–5 share its upper bytes and differ in the
  least significant byte (`addr`, `addr+1`, …). Frames on other addresses are ignored.
- Duplicate detection per pipe: a frame with the same PID and payload as the previous one on that
  pipe is acknowledged again but not delivered. Pitfall (real hardware too): several transmitters
  sharing one pipe whose consecutive frames happen to carry the same PID and payload are taken for
  a retransmission and dropped. The traffic generator puts the node id in the first payload byte.
- Unless NO_ACK is set, answer with an ACK on the same address 130 µs after the frame, carrying the
  pipe's ACK payload if one is configured. Return to RX 130 µs after the ACK.
- While transmitting an ACK or acting as PTX the node does not hear anything.

### Not modelled (yet)

Static payload length, CRC errors from noise (frames below sensitivity are simply not received),
RX/TX FIFOs deeper than one payload, ACK-payload FIFO depletion, interrupts/IRQ timing, power-down
and current consumption.

### Stack config

| Key | Default | Meaning |
|---|---|---|
| `addr` | `0xE7E7E7E700 + node id` | pipe-1 address (integer) |
| `aw` | 5 | address width, 3–5 bytes |
| `pipes` | 1 | number of RX pipes (1–5) |
| `listen` | 1 | stay in RX when not transmitting (PRX) |
| `send_to` | — | destination address |
| `send_every_ms`, `jitter_ms` | 0, 0 | traffic generator, as for the Kervan stack |
| `len` | 8 | payload bytes, 0–32 |
| `no_ack` | 0 | send with NO_ACK |
| `ard_us` | 500 | 250–4000, multiple of 250 |
| `arc` | 3 | 0–15 |
| `ack_payload_len` | 0 | bytes of ACK payload the PRX attaches (0–32) |

Events: `esb_tx` (new payload), `esb_retx`, `esb_tx_ds`, `esb_max_rt`, `esb_ack_payload`,
`esb_rx`, `esb_rx_dup`, `esb_tx_busy`.
