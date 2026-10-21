/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import { describe, expect, it } from 'vitest';

import type { TraceLine } from '../api';
import {
  appCounts,
  appendTrace,
  between,
  bins,
  buildReplay,
  emptyReplay,
  formatTime,
  hardwareComparison,
  lowerBound,
  nextEventTime,
  parseDetail,
  receptionsOf,
  txOfReception,
  txOverlapping,
} from './replay';

// A short trace in the format ksim writes.
const trace: TraceLine[] = [
  { t: 0, ev: 'meta', ksim: '0.1.0', abi: 3 },
  { t: 1000, ev: 'tx', node: 1, len: 10, airtime: 50000 },
  { t: 1000, ev: 'app', node: 1, name: 'app_tx', detail: 'to=2 seq=0' },
  { t: 51000, ev: 'rx', node: 2, from: 1, rssi: -90.5, snr: 6.25 },
  { t: 51000, ev: 'lost', node: 3, from: 1, reason: 'collision' },
  { t: 60000, ev: 'tx', node: 2, len: 2, airtime: 80000, data: '0a0b' },
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
    expect(r.txs[0]).toEqual({ t: 1000, end: 51000, node: 1, len: 10, relay: false, data: '' });
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

  it('builds the same replay from pieces, as a live stream delivers it', () => {
    for (let cut = 0; cut <= trace.length; cut++) {
      const live = emptyReplay(1_000_000);
      appendTrace(live, trace.slice(0, cut));
      appendTrace(live, trace.slice(cut));
      expect(live).toEqual(r);
    }
  });

  it('links frames and receptions', () => {
    const second = r.txs[1]!;
    expect(second.data).toBe('0a0b');
    expect(receptionsOf(r, second).map((x) => [x.node, x.reason])).toEqual([
      [1, 'half_duplex'],
      [3, 'rx_off'],
    ]);
    expect(txOfReception(r, r.rxs[0]!)).toBe(r.txs[0]);
    expect(txOfReception(r, r.rxs[3]!)).toBe(second);
    expect(txOfReception(r, { t: 51000, node: 2, from: 9, ok: true })).toBeUndefined();
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

describe('real boards', () => {
  // two boards (docs/hardware.md): node 2 sends, node 1 receives; the simulation runs alongside
  const hwTrace: TraceLine[] = [
    { t: 0, ev: 'hw', node: 1, kind: 'hello', port: 'COM5', board: 'rpi-pico', id: 'E660', fw: 'kervan-nrf24', version: 1 },
    { t: 0, ev: 'hw', node: 2, kind: 'hello', port: 'COM6', board: 'rpi-pico', id: 'E661', fw: 'kervan-nrf24', version: 1 },
    { t: 1000, ev: 'app', node: 2, name: 'esb_tx', detail: 'to=0xe7 pid=1 len=2' },
    { t: 1500, ev: 'app', node: 2, name: 'esb_tx_ds', detail: 'pid=1' },
    { t: 1600, ev: 'app', node: 1, name: 'esb_rx', detail: 'pipe=1 pid=1 len=2' },
    { t: 2000, ev: 'hw', node: 2, kind: 'tx', seq: 0, data: '0A0B', ack: true, retries: 1, ack_data: 'A0' },
    { t: 2400, ev: 'hw', node: 1, kind: 'rx', pipe: 1, data: '0A0B', rpd: true },
    // the receiver's clock is a little behind: its report comes first
    { t: 90000, ev: 'hw', node: 1, kind: 'rx', pipe: 1, data: '0C0D', rpd: false },
    { t: 90300, ev: 'hw', node: 2, kind: 'tx', seq: 1, data: '0C0D', ack: true, retries: 0 },
    { t: 200000, ev: 'hw', node: 2, kind: 'tx', seq: 2, data: '0E0F', ack: false, retries: 3 },
    { t: 300000, ev: 'hw', node: 2, kind: 'tx', seq: 3, data: '0E0F', ack: false, no_ack: true, retries: 0 },
    { t: 300500, ev: 'app', node: 2, name: 'esb_max_rt', detail: 'pid=2' },
    { t: 400000, ev: 'hw', node: 2, kind: 'error', msg: 'disconnected' },
  ];

  it('keeps boards and their reports', () => {
    const r = buildReplay(hwTrace, 1e6);
    expect(r.boards.map((b) => b.node + '@' + b.port)).toEqual(['1@COM5', '2@COM6']);
    expect(r.hw.map((e) => e.kind)).toEqual(['tx', 'rx', 'rx', 'tx', 'tx', 'tx', 'error']);
    expect(r.hw[0]).toMatchObject({ node: 2, data: '0A0B', ack: true, retries: 1, ackData: 'A0', peer: 1 });
    expect(r.hw[1]).toMatchObject({ node: 1, pipe: 1, rpd: true, peer: 2 });
    expect(r.hw[2]!.peer).toBe(2); // matched although it came first
    expect(r.hw[3]!.peer).toBe(1);
    expect(r.hw[4]!.peer).toBeUndefined(); // nobody received it
    expect(r.hw[5]).toMatchObject({ noAck: true, ack: false });
    expect(r.hw[6]!.msg).toBe('disconnected');
    expect(r.txs).toEqual([]); // reports are not simulated frames
  });

  it('pairs a reception only once and only with another board', () => {
    const r = emptyReplay(1e6);
    appendTrace(r, [
      { t: 1000, ev: 'hw', node: 2, kind: 'tx', data: '01', ack: true, retries: 0 },
      { t: 1100, ev: 'hw', node: 1, kind: 'rx', data: '01' },
      { t: 1200, ev: 'hw', node: 3, kind: 'rx', data: '01' },
      { t: 1300, ev: 'hw', node: 2, kind: 'rx', data: '01' },
      { t: 99000, ev: 'hw', node: 4, kind: 'rx', data: '01' },
    ]);
    expect(r.hw.map((e) => e.peer)).toEqual([1, 2, undefined, undefined, undefined]);
  });

  it('compares the boards with the simulation', () => {
    const c = hardwareComparison(buildReplay(hwTrace, 1e6));
    expect(c.map((x) => x.node)).toEqual([1, 2]);
    expect(c[1]!.real).toEqual({ sent: 4, acked: 2, failed: 1, retries: 4, received: 0 });
    expect(c[1]!.sim).toEqual({ sent: 1, acked: 1, failed: 1, retries: 0, received: 0 });
    expect(c[0]!.real.received).toBe(2);
    expect(c[0]!.sim.received).toBe(1);
    expect(hardwareComparison(buildReplay(trace, 1e6))).toEqual([]);
  });
});
