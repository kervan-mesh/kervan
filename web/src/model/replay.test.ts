/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import { describe, expect, it } from 'vitest';

import type { TraceLine } from '../api';
import {
  appCounts,
  between,
  bins,
  buildReplay,
  formatTime,
  lowerBound,
  nextEventTime,
  parseDetail,
  txOverlapping,
} from './replay';

// A short trace in the format ksim writes.
const trace: TraceLine[] = [
  { t: 0, ev: 'meta', ksim: '0.1.0', abi: 3 },
  { t: 1000, ev: 'tx', node: 1, len: 10, airtime: 50000 },
  { t: 1000, ev: 'app', node: 1, name: 'app_tx', detail: 'to=2 seq=0' },
  { t: 51000, ev: 'rx', node: 2, from: 1, rssi: -90.5, snr: 6.25 },
  { t: 51000, ev: 'lost', node: 3, from: 1, reason: 'collision' },
  { t: 60000, ev: 'tx', node: 2, len: 20, airtime: 80000 },
  { t: 60000, ev: 'app', node: 2, name: 'kv_relay', detail: '' },
  { t: 140000, ev: 'lost', node: 1, from: 2, reason: 'half_duplex' },
  { t: 140000, ev: 'lost', node: 3, from: 2, reason: 'rx_off' },
  { t: 900000, ev: 'app', node: 1, name: 'kv_stats', detail: 'tx=2 airtime_ms=130 junk' },
  { t: 900000, ev: 'future_event', node: 1 },
];

describe('buildReplay', () => {
  const r = buildReplay(trace, 1_000_000);

  it('sorts the trace into frames, receptions and app events', () => {
    expect(r.meta.abi).toBe(3);
    expect(r.txs).toHaveLength(2);
    expect(r.txs[0]).toEqual({ t: 1000, end: 51000, node: 1, len: 10, relay: false });
    expect(r.txs[1]!.relay).toBe(true);
    expect(r.maxAirtime).toBe(80000);
    expect(r.rxs.filter((x) => x.ok)).toHaveLength(1);
    expect(r.rxs[1]).toMatchObject({ ok: false, reason: 'collision', from: 1, node: 3 });
    expect(r.apps.map((a) => a.name)).toEqual(['app_tx', 'kv_relay', 'kv_stats']);
  });

  it('answers time queries', () => {
    expect(lowerBound(r.rxs, 51000)).toBe(0);
    expect(lowerBound(r.rxs, 51001)).toBe(2);
    expect(between(r.rxs, 51000, 139999)).toHaveLength(2);
    expect(txOverlapping(r, 52000, 59000)).toHaveLength(0);
    expect(txOverlapping(r, 50000, 70000).map((t) => t.node)).toEqual([1, 2]);
    expect(nextEventTime(r, 1000, 1)).toBe(51000);
    expect(nextEventTime(r, 51000, -1)).toBe(1000);
    expect(nextEventTime(r, 140000, 1)).toBeUndefined();
    expect(nextEventTime(r, 1000, -1)).toBeUndefined();
  });

  it('bins radio activity', () => {
    const b = bins(r, 10);
    expect(b).toHaveLength(10);
    expect(b[0]).toMatchObject({ start: 0, tx: 2, ok: 1, collision: 1, halfDuplex: 0, rxOff: 0 });
    expect(b[1]).toMatchObject({ start: 100000, halfDuplex: 1, rxOff: 1 });
    expect(b.slice(1).every((x) => x.tx === 0)).toBe(true);
  });

  it('counts app events per node and parses details', () => {
    const c = appCounts(r);
    expect(c.get(1)?.get('app_tx')).toBe(1);
    expect(c.get(2)?.get('kv_relay')).toBe(1);
    expect(parseDetail('tx=2 airtime_ms=130 junk')).toEqual({ tx: '2', airtime_ms: '130' });
  });
});

describe('formatTime', () => {
  it('formats virtual time', () => {
    expect(formatTime(0)).toBe('00:00.000');
    expect(formatTime(61_234_000)).toBe('01:01.234');
    expect(formatTime(3_600_000_000 + 5_000)).toBe('1:00:00.005');
  });
});
