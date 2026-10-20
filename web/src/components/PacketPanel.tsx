/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Packet inspector: one frame of a run — who heard it and how, its fields as the sending stack
// decodes them, and its bytes. Hovering a field highlights its bytes and the other way round.

import { useEffect, useState } from 'react';

import { decodeFrame, type FrameField } from '../api';
import { asciiChar, byteHex, bytesFromHex, dumpRows } from '../model/hex';
import { formatTime, LOSS_LABEL, receptionsOf, type Replay, type Tx } from '../model/replay';
import { findNode, type ScenarioDoc } from '../model/scenario';

interface Props {
  replay: Replay;
  tx: Tx;
  doc: ScenarioDoc; // the scenario of the run (sender's stack and config)
  onSeek: (t: number) => void;
  onSelectNode: (id: number) => void;
}

type Range = { offset: number; len: number } | null;

function covers(f: FrameField, i: number): boolean {
  return f.offset !== undefined && i >= f.offset && i < f.offset + (f.len ?? 0);
}

export function PacketPanel({ replay, tx, doc, onSeek, onSelectNode }: Props) {
  const sender = findNode(doc, tx.node);
  const [fields, setFields] = useState<FrameField[] | null | undefined>(undefined);
  const [error, setError] = useState<string | null>(null);
  const [hover, setHover] = useState<Range>(null);
  const bytes = bytesFromHex(tx.data);

  useEffect(() => {
    setFields(undefined);
    setError(null);
    setHover(null);
    if (!sender || !tx.data) {
      setFields(null);
      return;
    }
    let live = true;
    decodeFrame(sender.stack, sender.config, tx.data)
      .then((r) => live && setFields(r.fields))
      .catch((e: unknown) => live && setError(e instanceof Error ? e.message : String(e)));
    return () => {
      live = false;
    };
  }, [tx, sender]);

  const receptions = receptionsOf(replay, tx);
  const ok = receptions.filter((r) => r.ok).length;

  return (
    <div className="panel">
      <div className="inline" style={{ justifyContent: 'space-between' }}>
        <strong style={{ flex: 'none' }}>
          Frame from node {tx.node}
          {tx.relay && <span style={{ color: 'var(--relay)' }}> (relay)</span>}
        </strong>
        <button style={{ flex: 'none' }} onClick={() => onSeek(tx.t)}>
          Go to frame
        </button>
      </div>
      <table className="stats" style={{ marginTop: 8 }}>
        <tbody>
          <tr><td>sent</td><td>{formatTime(tx.t)}</td></tr>
          <tr><td>airtime</td><td>{((tx.end - tx.t) / 1000).toFixed(2)} ms</td></tr>
          <tr><td>length</td><td>{tx.len} bytes</td></tr>
          <tr><td>stack</td><td>{sender?.stack ?? '?'}</td></tr>
        </tbody>
      </table>

      <h3>Heard by ({ok} received, {receptions.length - ok} lost)</h3>
      {receptions.length === 0 ? (
        <p className="hint">Nobody was in range (or listening on this channel).</p>
      ) : (
        <table className="stats">
          <tbody>
            {receptions.map((r) => (
              <tr key={r.node} onClick={() => onSelectNode(r.node)}>
                <td>node {r.node}</td>
                <td style={{ color: r.ok ? 'var(--ok)' : 'var(--danger)' }}>
                  {r.ok
                    ? `✓ ${r.rssi} dBm, SNR ${r.snr} dB`
                    : `✗ ${LOSS_LABEL[r.reason ?? ''] ?? r.reason}`}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}

      <h3>Fields</h3>
      {error && <p className="hint" style={{ color: 'var(--danger)' }}>{error}</p>}
      {fields === undefined && !error && <p className="hint">Decoding…</p>}
      {fields === null && (
        <p className="hint">
          {tx.data ? `${sender?.stack ?? 'The stack'} does not describe this frame.` : 'This trace has no frame bytes (older format).'}
        </p>
      )}
      {fields && (
        <table className="stats">
          <tbody>
            {fields.map((f, i) => {
              const hl =
                hover !== null && f.offset !== undefined &&
                f.offset < hover.offset + hover.len && hover.offset < f.offset + (f.len ?? 0);
              return (
                <tr
                  key={`${f.name}-${i}`}
                  className={hl ? 'hl' : undefined}
                  onMouseEnter={() => setHover(f.offset !== undefined ? { offset: f.offset, len: f.len ?? 0 } : null)}
                  onMouseLeave={() => setHover(null)}
                >
                  <td>{f.name}</td>
                  <td>
                    {f.value}
                    {/* "N bytes" fields: show the bytes themselves */}
                    {/^\d+ bytes$/.test(f.value) && f.offset !== undefined && (f.len ?? 0) > 0 && (
                      <div className="mono">
                        {bytes
                          .slice(f.offset, f.offset + Math.min(f.len ?? 0, 24))
                          .map(byteHex)
                          .join(' ')}
                        {(f.len ?? 0) > 24 ? ' …' : ''}
                      </div>
                    )}
                  </td>
                </tr>
              );
            })}
          </tbody>
        </table>
      )}

      {bytes.length > 0 && (
        <>
          <h3>Bytes</h3>
          <div className="dump">
            {dumpRows(bytes).map((row) => (
              <div key={row.offset}>
                <span className="off">{row.offset.toString(16).padStart(4, '0')}</span>
                {row.bytes.map((b, k) => {
                  const i = row.offset + k;
                  const hl = hover !== null && i >= hover.offset && i < hover.offset + hover.len;
                  return (
                    <span
                      key={i}
                      className={`b ${hl ? 'hl' : ''}`}
                      onMouseEnter={() => {
                        const f = fields?.find((x) => covers(x, i) && x.len !== undefined && x.len < bytes.length);
                        setHover(f && f.offset !== undefined ? { offset: f.offset, len: f.len ?? 0 } : { offset: i, len: 1 });
                      }}
                      onMouseLeave={() => setHover(null)}
                    >
                      {byteHex(b)}
                    </span>
                  );
                })}
                <span className="ascii">{row.bytes.map(asciiChar).join('')}</span>
              </div>
            ))}
          </div>
        </>
      )}
    </div>
  );
}
