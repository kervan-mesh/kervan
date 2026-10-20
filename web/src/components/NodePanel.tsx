/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// One node: position, board, protocol stack and its settings, radio overrides, and (after a
// run) what happened to it.

import type { Catalog, NodeSummary } from '../api';
import { parseDetail, type Replay } from '../model/replay';
import {
  effectiveRadio,
  setKey,
  stacksFor,
  withField,
  without,
  type NodeDoc,
  type ScenarioDoc,
} from '../model/scenario';
import { KeyValueForm, SchemaForm } from './SchemaForm';

interface Props {
  doc: ScenarioDoc;
  node: NodeDoc;
  catalog: Catalog;
  stats: NodeSummary | undefined;
  replay: Replay | null;
  durationUs: number;
  onChange: (node: NodeDoc) => void;
  onRename: (id: number) => void;
  onDelete: () => void;
}

function NodeResults({ id, stats, replay, durationUs }: {
  id: number;
  stats: NodeSummary;
  replay: Replay | null;
  durationUs: number;
}) {
  const counts = new Map<string, number>();
  const details: { name: string; values: Record<string, string> }[] = [];
  for (const a of replay?.apps ?? []) {
    if (a.node !== id) {
      continue;
    }
    if (a.name.endsWith('_stats')) {
      details.push({ name: a.name, values: parseDetail(a.detail) });
    } else if (a.name !== 'kv_log') {
      counts.set(a.name, (counts.get(a.name) ?? 0) + 1);
    }
  }
  const duty = durationUs > 0 ? (100 * stats.airtime_us) / durationUs : 0;
  return (
    <>
      <h3>Radio results</h3>
      <table className="stats">
        <tbody>
          <tr><td>frames sent</td><td>{stats.tx}</td></tr>
          <tr><td>airtime</td><td>{(stats.airtime_us / 1e6).toFixed(2)} s ({duty.toFixed(3)} %)</td></tr>
          <tr><td>received</td><td>{stats.rx_ok}</td></tr>
          <tr><td>lost: collision</td><td>{stats.lost_collision}</td></tr>
          <tr><td>lost: half-duplex</td><td>{stats.lost_half_duplex}</td></tr>
          <tr><td>lost: receiver off</td><td>{stats.lost_rx_off}</td></tr>
        </tbody>
      </table>
      {counts.size > 0 && (
        <>
          <h3>Stack events</h3>
          <table className="stats">
            <tbody>
              {[...counts].sort().map(([k, v]) => (
                <tr key={k}><td>{k}</td><td>{v}</td></tr>
              ))}
            </tbody>
          </table>
        </>
      )}
      {details.map((d) => (
        <div key={d.name}>
          <h3>{d.name} (end of run)</h3>
          <table className="stats">
            <tbody>
              {Object.entries(d.values).map(([k, v]) => (
                <tr key={k}><td>{k}</td><td>{v}</td></tr>
              ))}
            </tbody>
          </table>
        </div>
      ))}
    </>
  );
}

export function NodePanel(props: Props) {
  const { doc, node, catalog } = props;
  const radio = effectiveRadio(doc, node, catalog.devices, catalog.phys);
  const device = catalog.devices.find((d) => d.name === node.device);
  const stacks = stacksFor(radio.type, catalog.stacks);
  const stack = catalog.stacks.find((s) => s.name === node.stack);
  const compatible = stacks.some((s) => s.name === node.stack);
  const set = (n: NodeDoc) => props.onChange(n);

  const coord = (i: number, text: string) => {
    const v = Number(text);
    if (text.trim() !== '' && Number.isFinite(v)) {
      const pos = [...node.pos];
      pos[i] = v;
      set({ ...node, pos });
    }
  };

  let powerHint = '';
  if (device?.limits.tx_power_levels) {
    powerHint = `${device.name}: ${device.limits.tx_power_levels.join(', ')} dBm`;
  } else if (device?.limits.tx_power_dbm) {
    powerHint = `${device.name}: ${device.limits.tx_power_dbm.min} … ${device.limits.tx_power_dbm.max} dBm`;
  }

  return (
    <div className="panel">
      <div className="inline" style={{ justifyContent: 'space-between' }}>
        <strong style={{ flex: 'none' }}>Node {node.id}</strong>
        <button className="danger" style={{ flex: 'none' }} onClick={props.onDelete}>
          Delete
        </button>
      </div>

      <h3>Place</h3>
      <div className="row">
        <label>id</label>
        <input
          type="number"
          min={0}
          defaultValue={node.id}
          key={node.id}
          onBlur={(e) => {
            const id = Number(e.target.value);
            if (Number.isInteger(id) && id >= 0 && id !== node.id) {
              props.onRename(id);
            }
          }}
        />
      </div>
      <div className="row">
        <label>x, y (m)</label>
        <div className="inline">
          <input type="number" value={node.pos[0] ?? 0} onChange={(e) => coord(0, e.target.value)} />
          <input type="number" value={node.pos[1] ?? 0} onChange={(e) => coord(1, e.target.value)} />
        </div>
      </div>

      <h3>Board and protocol</h3>
      <div className="row">
        <label>device</label>
        <select
          value={node.device ?? ''}
          onChange={(e) => set(withField(node, 'device', e.target.value || undefined))}
        >
          <option value="">(scenario radio)</option>
          {catalog.devices.map((d) => (
            <option key={d.name} value={d.name}>
              {d.name} — {d.radio.type}
            </option>
          ))}
        </select>
        {device && <div className="help">{device.description}</div>}
      </div>
      <div className="row">
        <label>stack</label>
        <select
          value={node.stack}
          onChange={(e) => set({ ...withField(node, 'config', undefined), stack: e.target.value })}
        >
          {!compatible && <option value={node.stack}>{node.stack} (needs another radio)</option>}
          {stacks.map((s) => (
            <option key={s.name} value={s.name}>
              {s.name} v{s.version}
            </option>
          ))}
        </select>
        {!compatible && (
          <div className="help" style={{ color: 'var(--danger)' }}>
            {node.stack} does not run on a {radio.type} radio
          </div>
        )}
      </div>
      {node.process !== undefined && (
        <p className="hint">This node’s stack runs in its own process (“process” in the file).</p>
      )}

      <h3>Radio ({radio.type})</h3>
      <SchemaForm
        options={catalog.phys[radio.type]?.options ?? []}
        values={without(node.radio, 'type')}
        inherited={radio.inherited}
        onChange={(k, v) => set(withField(node, 'radio', setKey(node.radio, k, v)))}
      />
      {powerHint && <p className="hint">tx power: {powerHint}</p>}

      {stack?.schema ? (
        <SchemaForm
          grouped
          groupPrefix={node.stack}
          options={stack.schema.options}
          values={node.config}
          onChange={(k, v) => set(withField(node, 'config', setKey(node.config, k, v)))}
        />
      ) : (
        <>
          <h3>Config</h3>
          <KeyValueForm
            values={node.config}
            onChange={(k, v) => set(withField(node, 'config', setKey(node.config, k, v)))}
          />
        </>
      )}

      {props.stats && (
        <NodeResults
          id={node.id}
          stats={props.stats}
          replay={props.replay}
          durationUs={props.durationUs}
        />
      )}
    </div>
  );
}
