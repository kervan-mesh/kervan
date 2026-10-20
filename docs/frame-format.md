# Kervan frame format (wire version 0)

Status: **draft** — implemented in `core/src/frame.c`, may still change before 1.0.

A *frame* is one radio packet. Kervan is designed around LoRa first, where every byte costs
~25–30 ms of airtime at SF12/BW125 and the regional payload limit can be as low as ~51 bytes.
The header is therefore kept to **8 bytes** (+1 byte only for fragmented messages).

## Layout

All multi-byte fields are **little-endian**. Bit fields are listed MSB first.
Frames are always (de)serialized byte by byte, never via struct layout.

```
Byte 0:   ver:2 | type:4 | ack_req:1 | frag:1
Byte 1:   hop_limit:3 | hop_count:3 | sec:1 | rsv:1
Byte 2:   net_id
Byte 3-4: dst          (u16 LE, 0xFFFF = broadcast)
Byte 5-6: src          (u16 LE, originator)
Byte 7:   seq          (u8, per-originator message counter)
[Byte 8:  index:4 | last:4]      present only if frag = 1
[payload ...]
[MIC]                            present only if sec = 1 (reserved, phase 7)
```

There is no length or CRC field: the PHY/transport provides both (LoRa CRC, UDP checksum).
Integrity against tampering will come from the MIC once encryption lands.

## Fields

| Field | Bits | Meaning |
|---|---|---|
| `ver` | 2 | Wire format version. This document is version `0`. Frames with another version are dropped, never relayed. |
| `type` | 4 | How the rest of the frame is parsed (see below). Carries no application meaning. |
| `ack_req` | 1 | Destination must answer with an end-to-end `ACK`. Not allowed with broadcast `dst`. |
| `frag` | 1 | Fragment header byte follows the fixed header. |
| `hop_limit` | 3 | Remaining relays allowed. Relays decrement it and stop at 0. Default 3, max 7. |
| `hop_count` | 3 | Relays already taken. `hop_limit + hop_count` never exceeds 7. |
| `sec` | 1 | Secured frame (AES-CCM, MIC at the end). Reserved; rejected until phase 7. |
| `rsv` | 1 | Must be sent as 0, ignored on receive. |
| `net_id` | 8 | Network identifier. Frames from other networks are dropped and never relayed. |
| `dst` | 16 | Final destination address. `0xFFFF` = broadcast. |
| `src` | 16 | Originator address. Must not be `0xFFFF`. |
| `seq` | 8 | Message id, incremented once per message (not per fragment). |

## Types

| Value | Type | Payload |
|---|---|---|
| `0x0` | `DATA` | Topic id (u16 LE) followed by the application data (see [messaging.md](messaging.md)). |
| `0x1` | `ACK` | 1 byte: the `seq` being acknowledged. |
| `0x2` | `CTRL` | 1 byte subtype + subtype data (PING/PONG first; BEACON, JOIN, ROUTE later). |
| `0x3`–`0xE` | reserved | Candidates: REQ/RESP, ROUTE. |
| `0xF` | `EXT` | Next byte is an extended type. Insurance; not used yet. |

## Fragmentation

- A message too large for one frame is split into at most **16 fragments**.
- Every fragment carries the same header (including `seq` and `ack_req`); only byte 8 differs:
  `index` (0-based) and `last` (index of the final fragment, i.e. count − 1).
- Knowing `last` from any fragment lets the receiver track arrivals with a 16-bit bitmap regardless
  of order, and reject a message that will not fit its buffer immediately.
- Payload is opaque to fragmentation: a topic id lives at the start of the *reassembled* payload.
- Relays do **not** reassemble; they forward fragments individually and stay stateless.
- Lost fragment: when no fragment arrived for 30 s the partial message is dropped (the timeout
  restarts with every fragment, since a duty cycle paces them). With `ack_req`, the sender
  retransmits the whole message. Selective repair (fragment bitmap in the ACK) may come later.
- Limits are compile-time: `KV_MAX_MSG_SIZE`, `KV_REASSEMBLY_SLOTS`. When slots are full, new
  messages are rejected (evicting partial ones would be an easy DoS).

## Relay and duplicate rules

- Duplicate key is `(src, seq)`, or `(src, seq, index)` for fragments.
- A relay forwards a frame if `ver` and `net_id` match, it is not a duplicate, it is not addressed
  to the relay itself and `hop_limit > 0`. It does **not** look at `type`, so old relays carry frame
  types they do not understand. Timing, suppression and budget: [messaging.md](messaging.md#mesh-managed-flooding).
- An unknown `type` reaching its destination is silently dropped.

## Design decisions

| Decision | Alternatives | Why |
|---|---|---|
| 8-byte header | Meshtastic 16 B, LoRaWAN ~13 B | ~9 bytes ≈ 250 ms less airtime per frame at SF12 than Meshtastic. |
| 16-bit addresses | 8-bit, 32-bit, EUI-64 | 255 nodes too few; 32/64-bit wastes airtime. EUI-64 travels only in JOIN. |
| `net_id` in header | LoRa sync word, key-derived | Sync word is LoRa-only; the protocol is transport-agnostic. Placed at byte 2 so foreign frames are dropped after 3 bytes. |
| Max 7 hops (3+3 bits) | 4+4 bits (15 hops) | Hop limit bounds flood radius, loops and latency (~1 s/hop at SF12). Beyond 7, use routing or gateways. To be validated in the simulator. |
| 8-bit `seq` | 12-bit | Time-bounded dedup cache makes wrap harmless at LoRa rates; keeps byte alignment. |
| End-to-end ACK | Hop-by-hop | Simplest; does not affect the header. |
| `seq` = message id, dedup by `(src, seq, index)` | One `seq` per fragment | Same `seq` per fragment with plain `(src, seq)` dedup would drop fragments 2..n; a seq per fragment burns the 8-bit space. |
| `index:4 | last:4` | `index:7 | more:1` | Total known from any fragment (out-of-order mesh delivery), early size check; 16 × 42 B ≈ 670 B at SF12 is plenty for LoRa. |
| Broadcast + `ack_req` invalid | Allow | Would trigger an ACK storm from every node. |
| Static addresses in phase 1 | Hashed EUI-64, JOIN | Address assignment is designed with the mesh (phase 4). |
