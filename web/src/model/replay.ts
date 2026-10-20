/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// A run's trace (JSON Lines, docs/simulator.md) turned into time-sorted arrays the viewer can
// query quickly at any virtual time. Nothing here is protocol specific: stacks report their own
// events as "app" lines, and those are shown by name.

import type { TraceLine } from '../api';

export interface Tx {
  t: number; // µs
  end: number;
  node: number;
  len: number;
  relay: boolean; // the stack reported a relay at the same instant
  data: string; // frame bytes, hex ("" in traces before format 2)
}

export type LossReason = 'collision' | 'half_duplex' | 'rx_off' | string;

export interface Rx {
  t: number; // end of the frame
  node: number; // receiver
  from: number;
  ok: boolean;
  reason?: LossReason;
  rssi?: number;
  snr?: number;
}

export interface AppEvent {
  t: number;
  node: number;
  name: string;
  detail: string;
}

export interface Replay {
  duration: number; // µs
  txs: Tx[];
  rxs: Rx[];
  apps: AppEvent[];
  maxAirtime: number;
  meta: Record<string, unknown>;
}

const RELAY_EVENTS = new Set(['kv_relay']);

export function buildReplay(trace: TraceLine[], duration: number): Replay {
  const txs: Tx[] = [];
  const rxs: Rx[] = [];
  const apps: AppEvent[] = [];
  let meta: Record<string, unknown> = {};
  let maxAirtime = 0;

  for (const line of trace) {
    const node = Number(line.node);
    switch (line.ev) {
      case 'meta':
        meta = line;
        break;
      case 'tx': {
        const airtime = Number(line.airtime);
        maxAirtime = Math.max(maxAirtime, airtime);
        txs.push({
          t: line.t,
          end: line.t + airtime,
          node,
          len: Number(line.len),
          relay: false,
          data: typeof line.data === 'string' ? line.data : '',
        });
        break;
      }
      case 'rx':
        rxs.push({
          t: line.t,
          node,
          from: Number(line.from),
          ok: true,
          rssi: Number(line.rssi),
          snr: Number(line.snr),
        });
        break;
      case 'lost':
        rxs.push({ t: line.t, node, from: Number(line.from), ok: false, reason: String(line.reason) });
        break;
      case 'app':
        apps.push({ t: line.t, node, name: String(line.name), detail: String(line.detail ?? '') });
        break;
      default:
        break; // newer trace events are ignored, not fatal
    }
  }

  // A stack reports a relay in the same instant it starts the frame.
  const relayAt = new Set(
    apps.filter((a) => RELAY_EVENTS.has(a.name)).map((a) => `${a.node}@${a.t}`),
  );
  for (const tx of txs) {
    tx.relay = relayAt.has(`${tx.node}@${tx.t}`);
  }
  return { duration, txs, rxs, apps, maxAirtime, meta };
}

// First index whose t is >= t (arrays are sorted by t).
export function lowerBound(items: { t: number }[], t: number): number {
  let lo = 0;
  let hi = items.length;
  while (lo < hi) {
    const mid = (lo + hi) >>> 1;
    if (items[mid]!.t < t) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return lo;
}

// Items with from <= t <= to.
export function between<T extends { t: number }>(items: T[], from: number, to: number): T[] {
  const out: T[] = [];
  for (let i = lowerBound(items, from); i < items.length && items[i]!.t <= to; i++) {
    out.push(items[i]!);
  }
  return out;
}

// Frames on air at some point in [from, to].
export function txOverlapping(r: Replay, from: number, to: number): Tx[] {
  const out: Tx[] = [];
  for (let i = lowerBound(r.txs, from - r.maxAirtime); i < r.txs.length; i++) {
    const tx = r.txs[i]!;
    if (tx.t > to) {
      break;
    }
    if (tx.end >= from) {
      out.push(tx);
    }
  }
  return out;
}

// The next / previous radio event (tx start or reception) after / before t.
export function nextEventTime(r: Replay, t: number, dir: 1 | -1): number | undefined {
  const candidates: number[] = [];
  for (const items of [r.txs, r.rxs] as { t: number }[][]) {
    if (dir > 0) {
      const i = lowerBound(items, t + 1);
      if (i < items.length) {
        candidates.push(items[i]!.t);
      }
    } else {
      const i = lowerBound(items, t) - 1;
      if (i >= 0) {
        candidates.push(items[i]!.t);
      }
    }
  }
  if (candidates.length === 0) {
    return undefined;
  }
  return dir > 0 ? Math.min(...candidates) : Math.max(...candidates);
}

export interface Bin {
  start: number;
  tx: number;
  ok: number;
  collision: number;
  halfDuplex: number;
  rxOff: number;
}

// Radio activity in `count` equal time bins.
export function bins(r: Replay, count: number): Bin[] {
  const width = Math.max(1, r.duration / count);
  const out: Bin[] = Array.from({ length: count }, (_, i) => ({
    start: i * width,
    tx: 0,
    ok: 0,
    collision: 0,
    halfDuplex: 0,
    rxOff: 0,
  }));
  const at = (t: number) => out[Math.min(count - 1, Math.floor(t / width))]!;
  for (const tx of r.txs) {
    at(tx.t).tx++;
  }
  for (const rx of r.rxs) {
    const b = at(rx.t);
    if (rx.ok) {
      b.ok++;
    } else if (rx.reason === 'collision') {
      b.collision++;
    } else if (rx.reason === 'half_duplex') {
      b.halfDuplex++;
    } else {
      b.rxOff++;
    }
  }
  return out;
}

// App event counts per node: node -> name -> count.
export function appCounts(r: Replay): Map<number, Map<string, number>> {
  const out = new Map<number, Map<string, number>>();
  for (const a of r.apps) {
    let m = out.get(a.node);
    if (!m) {
      m = new Map();
      out.set(a.node, m);
    }
    m.set(a.name, (m.get(a.name) ?? 0) + 1);
  }
  return out;
}

// "a=1 b=x" -> { a: "1", b: "x" }; words without '=' are ignored.
export function parseDetail(detail: string): Record<string, string> {
  const out: Record<string, string> = {};
  for (const word of detail.split(/\s+/)) {
    const eq = word.indexOf('=');
    if (eq > 0) {
      out[word.slice(0, eq)] = word.slice(eq + 1);
    }
  }
  return out;
}

export function formatTime(us: number): string {
  const totalMs = Math.floor(us / 1000);
  const ms = totalMs % 1000;
  const s = Math.floor(totalMs / 1000) % 60;
  const m = Math.floor(totalMs / 60000) % 60;
  const h = Math.floor(totalMs / 3600000);
  const pad = (v: number, n = 2) => String(v).padStart(n, '0');
  return `${h > 0 ? `${h}:` : ''}${pad(m)}:${pad(s)}.${pad(ms, 3)}`;
}

export const LOSS_LABEL: Record<string, string> = {
  collision: 'collision',
  half_duplex: 'half-duplex (was transmitting)',
  rx_off: 'receiver off',
};

// The frame a reception (or loss) belongs to: sent by rx.from, ending when rx happened.
export function txOfReception(r: Replay, rx: Rx): Tx | undefined {
  for (let i = lowerBound(r.txs, rx.t - r.maxAirtime); i < r.txs.length; i++) {
    const tx = r.txs[i]!;
    if (tx.t > rx.t) {
      break;
    }
    if (tx.node === rx.from && tx.end === rx.t) {
      return tx;
    }
  }
  return undefined;
}

// Everyone who heard a frame, and how it went.
export function receptionsOf(r: Replay, tx: Tx): Rx[] {
  return between(r.rxs, tx.end, tx.end).filter((rx) => rx.from === tx.node);
}
