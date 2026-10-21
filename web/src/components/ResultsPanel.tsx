/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// The outcome of a run: totals, per-node radio statistics with duty cycle, stack events per
// node, and the model versions that produced it.

import { Fragment } from 'react';

import type { RunResult } from '../api';
import { appCounts, hardwareComparison, type EsbCounts, type Replay } from '../model/replay';

interface Props {
  result: RunResult;
  replay: Replay;
  selected: number | null;
  onSelect: (id: number) => void;
}

const HIDDEN = new Set(['kv_stats', 'kv_log']);

function pct(n: number, d: number): string {
  return d > 0 ? `${((100 * n) / d).toFixed(1)} %` : '–';
}

// Real boards next to the simulation of the same nodes.
function BoardTable({ replay, selected, onSelect }: Omit<Props, 'result'>) {
  const rows = hardwareComparison(replay);
  if (rows.length === 0) {
    return null;
  }
  const cells = (c: EsbCounts) => (
    <>
      <td>{c.sent}</td>
      <td>{pct(c.acked, c.acked + c.failed)}</td>
      <td>{c.acked + c.failed > 0 ? (c.retries / (c.acked + c.failed)).toFixed(2) : '–'}</td>
      <td>{c.received}</td>
    </>
  );
  return (
    <>
      <h3>Real boards vs. simulation</h3>
      <table className="stats">
        <thead>
          <tr>
            <th>node</th>
            <th></th>
            <th title="frames sent">sent</th>
            <th title="acknowledged, of the frames that asked for an ACK">ACK</th>
            <th title="retransmissions per frame">retr.</th>
            <th title="frames received">recv</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((r) => (
            <Fragment key={r.node}>
              <tr className={r.node === selected ? 'selected' : undefined}
                onClick={() => onSelect(r.node)} title={`${r.board.board} ${r.board.id} on ${r.board.port}`}>
                <td rowSpan={2}>{r.node}</td>
                <td className="hw">real</td>
                {cells(r.real)}
              </tr>
              <tr className={r.node === selected ? 'selected' : undefined}
                onClick={() => onSelect(r.node)}>
                <td>sim</td>
                {cells(r.sim)}
              </tr>
            </Fragment>
          ))}
        </tbody>
      </table>
      <p className="hint">
        Same nodes, same settings: differences come from the channel model (distance, walls,
        antennas) and from the boards. Phase 6d calibrates the model with such measurements.
      </p>
    </>
  );
}

function DutyBars({ result, selected, onSelect }: Omit<Props, 'replay'>) {
  const s = result.summary;
  const duties = s.nodes.map((n) => ({ id: n.id, duty: s.duration_us > 0 ? n.airtime_us / s.duration_us : 0 }));
  const max = Math.max(s.duty_cycle_limit * 1.25, ...duties.map((d) => d.duty), 1e-9);
  const rowH = 14;
  const w = 330;
  const labelW = 34;
  const valueW = 52; // right column with the percentages
  const scale = (w - labelW - valueW - 6) / max;
  const limitX = labelW + s.duty_cycle_limit * scale;
  return (
    <svg width="100%" viewBox={`0 0 ${w} ${duties.length * rowH + 14}`} role="img" aria-label="Duty cycle per node">
      {duties.map((d, i) => {
        const over = s.duty_cycle_limit > 0 && d.duty > s.duty_cycle_limit;
        return (
          <g key={d.id} transform={`translate(0 ${i * rowH})`} onClick={() => onSelect(d.id)} style={{ cursor: 'pointer' }}>
            <text x={0} y={10} fontSize={10} fill="var(--muted)" fontWeight={d.id === selected ? 700 : 400}>
              {d.id}
            </text>
            <rect x={labelW} y={2} width={Math.max(1, d.duty * scale)} height={rowH - 4} rx={2}
              fill={over ? 'var(--danger)' : 'var(--accent)'} opacity={d.id === selected ? 1 : 0.75} />
            <text x={w - 2} y={10} fontSize={10} fill={over ? 'var(--danger)' : 'var(--text)'} textAnchor="end">
              {(d.duty * 100).toFixed(2)} %
            </text>
          </g>
        );
      })}
      {s.duty_cycle_limit > 0 && (
        <>
          <line x1={limitX} x2={limitX} y1={0} y2={duties.length * rowH} stroke="var(--danger)" strokeDasharray="3 2" />
          <text x={limitX} y={duties.length * rowH + 11} fontSize={10} fill="var(--danger)" textAnchor="middle">
            limit {(s.duty_cycle_limit * 100).toFixed(2)} %
          </text>
        </>
      )}
    </svg>
  );
}

export function ResultsPanel({ result, replay, selected, onSelect }: Props) {
  const s = result.summary;
  const lost = s.lost_collision + s.lost_half_duplex + s.lost_rx_off;
  const ev = s.app_events;
  const sent = ev.app_tx ?? 0;
  const acked = ev.app_ack ?? 0;
  const timeouts = ev.app_ack_timeout ?? 0;
  const maxDuty = Math.max(0, ...s.nodes.map((n) => (s.duration_us > 0 ? n.airtime_us / s.duration_us : 0)));
  const over = s.duty_cycle_limit > 0 && maxDuty > s.duty_cycle_limit;

  const counts = appCounts(replay);
  const names = [...new Set([...counts.values()].flatMap((m) => [...m.keys()]))]
    .filter((n) => !HIDDEN.has(n))
    .sort();

  return (
    <div className="panel">
      {result.truncated && (
        <p className="hint" style={{ color: 'var(--warn)' }}>
          The trace was cut (too long): the replay ends early, the numbers below cover the whole run.
        </p>
      )}
      <div className="cards">
        <div className="card">
          <div className="value">{s.tx}</div>
          <div className="label">frames sent</div>
        </div>
        <div className="card">
          <div className="value">{s.rx_ok}</div>
          <div className="label">frames received ({pct(s.rx_ok, s.rx_ok + lost)} of heard)</div>
        </div>
        <div className="card">
          <div className="value">{lost}</div>
          <div className="label">
            lost: {s.lost_collision} collision, {s.lost_half_duplex} half-duplex, {s.lost_rx_off} rx off
          </div>
        </div>
        <div className="card">
          <div className={`value ${over ? 'over' : ''}`}>{(maxDuty * 100).toFixed(2)} %</div>
          <div className="label">busiest node’s duty cycle</div>
        </div>
        {sent > 0 && (
          <div className="card">
            <div className="value">{sent}</div>
            <div className="label">messages published ({ev.app_rx ?? 0} deliveries)</div>
          </div>
        )}
        {acked + timeouts > 0 && (
          <div className="card">
            <div className="value">{pct(acked, acked + timeouts)}</div>
            <div className="label">acknowledged ({acked} ok, {timeouts} timed out)</div>
          </div>
        )}
      </div>

      <BoardTable replay={replay} selected={selected} onSelect={onSelect} />

      <h3>Duty cycle per node</h3>
      <DutyBars result={result} selected={selected} onSelect={onSelect} />

      <h3>Radio per node</h3>
      <table className="stats">
        <thead>
          <tr>
            <th>node</th>
            <th title="frames sent">tx</th>
            <th title="frames received">rx</th>
            <th title="lost to collisions">coll.</th>
            <th title="lost while transmitting">h-d</th>
            <th title="lost with the receiver off">off</th>
          </tr>
        </thead>
        <tbody>
          {s.nodes.map((n) => (
            <tr key={n.id} className={n.id === selected ? 'selected' : undefined} onClick={() => onSelect(n.id)}>
              <td>{n.id}</td>
              <td>{n.tx}</td>
              <td>{n.rx_ok}</td>
              <td>{n.lost_collision}</td>
              <td>{n.lost_half_duplex}</td>
              <td>{n.lost_rx_off}</td>
            </tr>
          ))}
        </tbody>
      </table>

      {names.length > 0 && (
        <>
          <h3>Stack events per node</h3>
          <div style={{ overflowX: 'auto' }}>
            <table className="stats">
              <thead>
                <tr>
                  <th>node</th>
                  {names.map((n) => (
                    <th key={n} title={n}>{n.replace(/^(app|kv|esb)_/, '')}</th>
                  ))}
                </tr>
              </thead>
              <tbody>
                {s.nodes.map((n) => (
                  <tr key={n.id} className={n.id === selected ? 'selected' : undefined} onClick={() => onSelect(n.id)}>
                    <td>{n.id}</td>
                    {names.map((name) => (
                      <td key={name}>{counts.get(n.id)?.get(name) ?? ''}</td>
                    ))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      )}

      <h3>Reproduce</h3>
      <table className="stats">
        <tbody>
          <tr><td>seed</td><td>{s.seed}</td></tr>
          <tr><td>trace hash</td><td className="mono">{s.trace_hash}</td></tr>
          {Object.entries(s.versions).map(([k, v]) => (
            <tr key={k}><td>{k}</td><td>{v}</td></tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
