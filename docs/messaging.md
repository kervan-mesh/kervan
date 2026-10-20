# Kervan messaging: pub/sub, QoS, fragmentation, duty cycle, mesh

Status: **phases 3 and 4** — implemented in `core/src/node.c`, API in
[`core/include/kervan/node.h`](../core/include/kervan/node.h). Wire format:
[frame-format.md](frame-format.md).

## Publish / subscribe

- No broker. `kv_node_publish(node, dst, topic, data, len, qos, &seq)` sends to one node or to
  `KV_ADDR_BROADCAST`.
- A `DATA` payload is `topic (u16 LE) | data`.
- Delivery (`on_message`): unicasts to this node are always delivered; broadcasts only for topics
  the node subscribed to (`kv_node_subscribe`, up to `KV_MAX_SUBSCRIPTIONS`).
- **Topic ids** are 16-bit: `kv_topic_id("sensors/temp")` = FNV-1a 32-bit folded to 16 bits. Names
  never travel on air and need no registration; if two names of one network collide, use an
  explicit id for one of them.

## QoS

| QoS | Meaning | Destination |
|---|---|---|
| 0 | at most once, no ACK | unicast or broadcast |
| 1 | at least once: end-to-end ACK, retransmission with backoff, `on_ack(seq, KV_OK / KV_ERR_TIMEOUT)` | unicast only (a broadcast ACK would be a storm) |

- Own messages wait in a queue of `KV_TX_QUEUE` (default 4); a QoS 1 message stays at its head
  until it is acknowledged or gives up, so messages leave in order. Full queue → `KV_ERR_BUSY`.
- **Backoff:** the k-th wait for an ACK is `ack_timeout_ms · 2^(k-1)` plus a random
  `0 … ack_timeout_ms/2`, so nodes that lost the same frame do not retry in lockstep. After
  `max_retries` retransmissions the message fails with `KV_ERR_TIMEOUT`.
- A retransmission is recognised by `(src, seq)` and acknowledged again but not delivered again.
  Relayed copies of a message arrive shortly after the first one; a node acknowledges a repeated
  message only if its previous ACK for it is older than `max(2 · relay_window_ms, 1 s)`.

## Fragmentation

- A message (`2 + len` bytes, at most `KV_MAX_MSG_SIZE` = 256) that does not fit one frame is split
  into at most 16 fragments of `mtu − 9` bytes. All nodes of a network must use the same MTU.
- All fragments share the message's `seq`; QoS 1 acknowledges the complete message, and a retry
  resends all fragments.
- The receiver reassembles in `KV_REASSEMBLY_SLOTS` (default 2) buffers, in any order. A slot is
  freed when its message completes or when **no fragment arrived for `KV_REASSEMBLY_TIMEOUT_MS`**
  (30 s). The timeout restarts with every fragment because a duty cycle paces fragments far apart
  (at 1 % a 102 ms fragment is followed by ~10 s of silence). Live slots are never evicted.

## Duty-cycle budget

Radio regulations limit airtime (EU868: 1 % in most sub-bands). Each node keeps a budget so that
it stays within `duty_cycle_permille` (default 10 = 1 %; 0 disables it):

- **Token bucket.** Credit grows by `p/1000` ms per ms of time and is spent by the measured
  airtime (from `radio_send` to `on_tx_done`; no PHY knowledge needed). After sending `T` ms the
  node waits `T · (1000 − p) / p` — 99 × the airtime at 1 % — the classic duty-cycle rule.
- `duty_burst_ms` (default 0) lets idle time build credit for short bursts.
- **Own messages** wait for the budget (`budget_waits`).
- **ACKs** — own and relayed — may run into a debt of up to `ack_debt_ms` (default 500 ms of
  airtime), repaid as extra off-time: an ACK is tiny, and dropping it causes retransmissions that
  cost far more.
- **Relays** of data frames are dropped when the budget is empty (`relay_dropped`): a late relay is
  useless.

Over a long run a node uses at most `p` of the time plus one frame plus the ACK debt limit.

## Mesh: managed flooding

A node relays a frame it has not seen before if relaying is enabled, the frame is not addressed to
the node itself, and `hop_limit > 0`. The relayed copy keeps `src`, `dst`, `seq` and fragment
fields, with `hop_limit − 1` and `hop_count + 1`. Relays look at the header only, so they also carry
types they do not understand.

- **Weaker signal first.** A relay waits `relay_window_ms · w + jitter` with `w` going from 0 at
  SNR −20 dB to 1 at +10 dB (0.5 if the port reports no SNR). Distant nodes, which extend the
  coverage most, relay first.
- **Suppression.** While waiting, a node that hears the same frame relayed by another node with at
  least the same hop count cancels its own relay (`relay_cancelled`).
- **ACKs flood back** the same way to the original sender.
- Broadcasts are delivered locally *and* relayed; unicasts are relayed by everyone except the
  destination.

Known cost (visible in the simulator, `sim/scenarios/line_5.json`): flooding carries every
message to the whole network, so nodes at the edge spend budget on relays nobody needs and busy
relays starve their own traffic. Route learning (relaying only towards the destination) is the
next step once measurements call for it; it needs a next-hop field in the header.

## Resources (Cortex-M4, `-Os`, defaults)

| | |
|---|---|
| Code (frame + node + topic) | ≈ 4.2 KB flash |
| `kv_node_t` | ≈ 3.6 KB RAM (queue 4 × 256 B, relays 4 × 255 B, reassembly 2 × 256 B, dedup 32) |

All limits are `#define`s in `node.h` and can be lowered for smaller MCUs.

## Statistics

`kv_node_stats()` counts frames and airtime sent, frames received and duplicates, messages
delivered, ACKs sent, retransmissions, relays (done, cancelled, dropped), dropped reassemblies and
budget waits. The simulator's Kervan stack reports them as `kv_stats` at the end of a run.
