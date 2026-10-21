/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// What a node runs on: the scenario's radio, a board, or a radio module wired to an MCU — with
// the parts' key data and how to wire them.

import { useEffect, useState } from 'react';

import { getWiring, type Hardware, type Wiring } from '../api';
import { nodeHardware, withField, type NodeDoc } from '../model/scenario';

type Mode = 'none' | 'board' | 'parts';

interface Props {
  node: NodeDoc;
  hw: Hardware;
  onChange: (n: NodeDoc) => void;
}

function modeOf(n: NodeDoc): Mode {
  return n.device ? 'board' : n.module || n.mcu ? 'parts' : 'none';
}

function power(limits: { tx_power_dbm?: { min: number; max: number }; tx_power_levels?: number[] }) {
  if (limits.tx_power_levels) {
    return `${limits.tx_power_levels.join(' / ')} dBm`;
  }
  if (limits.tx_power_dbm) {
    return `${limits.tx_power_dbm.min} … ${limits.tx_power_dbm.max} dBm`;
  }
  return '–';
}

function WiringView({ board, mcu, module }: { board?: string; mcu?: string; module?: string }) {
  const [wiring, setWiring] = useState<Wiring | null>(null);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setWiring(null);
    setError(null);
    const req = board ? getWiring(board) : mcu && module ? getWiring(mcu, module) : null;
    if (!req) {
      return;
    }
    let live = true;
    req
      .then((w) => live && setWiring(w))
      .catch((e: unknown) => live && setError(e instanceof Error ? e.message : String(e)));
    return () => {
      live = false;
    };
  }, [board, mcu, module]);

  if (!board && !mcu) {
    return <p className="hint">Pick an MCU to see how to wire the module.</p>;
  }
  if (error) {
    return <p className="hint" style={{ color: 'var(--danger)' }}>{error}</p>;
  }
  if (!wiring) {
    return <p className="hint">Loading…</p>;
  }
  return (
    <div>
      {wiring.warnings.map((w) => (
        <p key={w} className="warning">⚠ {w}</p>
      ))}
      {wiring.integrated ? (
        <p className="hint">The radio is on the board: nothing to wire.</p>
      ) : (
        <table className="stats wiring">
          <thead>
            <tr>
              <th>module</th>
              <th>MCU pin</th>
            </tr>
          </thead>
          <tbody>
            {wiring.rows.map((r) => (
              <tr key={r.module_pin}>
                <td>{r.module_pin}</td>
                <td>
                  {r.mcu_pin || '–'}
                  {r.role === 'gpio_in_optional' && <span className="hint"> (optional)</span>}
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
      {wiring.notes.length > 0 && (
        <ul className="notes">
          {wiring.notes.map((n) => (
            <li key={n}>{n}</li>
          ))}
        </ul>
      )}
    </div>
  );
}

export function HardwarePicker({ node, hw, onChange }: Props) {
  const mode = modeOf(node);
  const parts = nodeHardware(node, hw);

  const clear = (n: NodeDoc) =>
    withField(withField(withField(n, 'device', undefined), 'mcu', undefined), 'module', undefined);

  const switchTo = (m: Mode) => {
    if (m === mode) {
      return;
    }
    if (m === 'none') {
      onChange(clear(node));
    } else if (m === 'board') {
      // keep the radio technology if a board has the same module
      const same = hw.boards.find((b) => b.module === parts.module?.name) ?? hw.boards[0];
      onChange({ ...clear(node), device: same?.name });
    } else {
      onChange({
        ...clear(node),
        ...(parts.mcu ? { mcu: parts.mcu.name } : {}),
        module: parts.module?.name ?? hw.modules[0]?.name,
      });
    }
  };

  return (
    <div>
      <div className="segmented" role="radiogroup" aria-label="Hardware">
        {(
          [
            ['none', 'Scenario radio'],
            ['board', 'Board'],
            ['parts', 'MCU + module'],
          ] as const
        ).map(([m, label]) => (
          <label key={m} className={mode === m ? 'on' : undefined}>
            <input type="radio" name="hw-mode" checked={mode === m} onChange={() => switchTo(m)} />
            {label}
          </label>
        ))}
      </div>

      {mode === 'none' && (
        <p className="hint">The node uses the scenario’s radio settings (Scenario tab), no hardware limits.</p>
      )}

      {mode === 'board' && (
        <div className="row">
          <label>board</label>
          <select value={node.device} onChange={(e) => onChange({ ...node, device: e.target.value })}>
            {parts.missing && <option value={node.device}>{node.device} (unknown)</option>}
            {hw.boards.map((b) => (
              <option key={b.name} value={b.name}>
                {b.title}
              </option>
            ))}
          </select>
        </div>
      )}

      {mode === 'parts' && (
        <>
          <div className="row">
            <label>MCU</label>
            <select
              value={node.mcu ?? ''}
              onChange={(e) => onChange(withField(node, 'mcu', e.target.value || undefined))}
            >
              <option value="">(none)</option>
              {hw.mcus.map((m) => (
                <option key={m.name} value={m.name}>
                  {m.title}
                </option>
              ))}
            </select>
          </div>
          <div className="row">
            <label>radio module</label>
            <select value={node.module ?? ''} onChange={(e) => onChange({ ...node, module: e.target.value })}>
              {hw.modules.map((m) => (
                <option key={m.name} value={m.name}>
                  {m.title}
                </option>
              ))}
            </select>
          </div>
        </>
      )}

      {parts.missing && (
        <p className="warning">⚠ “{parts.missing}” is not in the hardware catalog.</p>
      )}

      {(parts.mcu || parts.module) && (
        <table className="stats" style={{ marginTop: 6 }}>
          <tbody>
            {parts.mcu && (
              <tr title={parts.mcu.description}>
                <td>MCU</td>
                <td>
                  {parts.mcu.chip}, {parts.mcu.ram_kb} KB RAM, {parts.mcu.flash_kb} KB flash,{' '}
                  {parts.mcu.logic_v} V
                </td>
              </tr>
            )}
            {parts.module && (
              <>
                <tr title={parts.module.description}>
                  <td>radio</td>
                  <td>
                    {parts.module.chip} ({parts.module.radio.type})
                  </td>
                </tr>
                <tr>
                  <td>tx power</td>
                  <td>{power(parts.limits ?? parts.module.limits)}</td>
                </tr>
                {(parts.module.antenna_dbi !== 0 || parts.module.lna_db !== 0) && (
                  <tr>
                    <td>antenna / LNA</td>
                    <td>
                      {parts.module.antenna_dbi} dBi / +{parts.module.lna_db} dB
                    </td>
                  </tr>
                )}
                <tr>
                  <td>current</td>
                  <td>
                    tx {parts.module.current_ma.tx} mA, rx {parts.module.current_ma.rx} mA
                  </td>
                </tr>
              </>
            )}
          </tbody>
        </table>
      )}

      {mode !== 'none' && parts.module && (
        <details className="wiring-box" open>
          <summary>Wiring</summary>
          <WiringView board={node.device} mcu={parts.mcu?.name} module={parts.module.name} />
        </details>
      )}
    </div>
  );
}
