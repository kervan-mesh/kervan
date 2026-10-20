/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Scenario-wide settings: simulation, channel model, the radio of nodes without a device, and
// the node list.

import type { Catalog, OptionSpec } from '../api';
import {
  SCENARIO_DEFAULTS,
  setKey,
  without,
  type ConfigValue,
  type ScenarioDoc,
} from '../model/scenario';
import { SchemaForm } from './SchemaForm';

const SIM_OPTIONS: OptionSpec[] = [
  { key: 'duration_s', type: 'number', min: 0, help: 'Simulated time in seconds' },
  { key: 'seed', type: 'int', min: 0, help: 'Same seed, same results' },
  { key: 'mtu', type: 'int', min: 1, max: 255, help: 'Largest frame in bytes (LoRa: 51 at SF12)' },
  {
    key: 'duty_cycle_limit',
    type: 'number',
    min: 0,
    max: 1,
    help: 'Regulatory limit to check against: 0.01 = 1 %',
  },
  {
    key: 'capture_threshold_db',
    type: 'number',
    help: 'LoRa: a frame survives a collision if this much stronger',
  },
  { key: 'noise_figure_db', type: 'number', help: 'Receiver noise figure' },
];

const PATH_LOSS_OPTIONS: OptionSpec[] = [
  {
    key: 'exponent',
    type: 'number',
    min: 1,
    max: 6,
    default: 2.7,
    help: '2 open field, 2.7-3.5 urban, 4+ indoors',
  },
  {
    key: 'shadowing_sigma_db',
    type: 'number',
    min: 0,
    default: 0,
    help: 'Per-link random variation (obstacles)',
  },
  { key: 'ref_distance_m', type: 'number', min: 0.001, default: 1, help: 'Reference distance d0' },
  { key: 'ref_loss_db', type: 'number', help: 'Loss at d0 (default: free space)' },
];

interface Props {
  doc: ScenarioDoc;
  catalog: Catalog;
  selected: number | null;
  onChange: (doc: ScenarioDoc) => void;
  onSelect: (id: number) => void;
}

export function ScenarioPanel({ doc, catalog, selected, onChange, onSelect }: Props) {
  const radioType = typeof doc.radio?.type === 'string' ? doc.radio.type : 'lora';
  const phyOptions = catalog.phys[radioType]?.options ?? [];
  const withoutDevice = doc.nodes.filter((n) => !n.device).length;

  const setTop = (key: string, v: ConfigValue | undefined) => {
    const next = { ...doc } as Record<string, unknown>;
    if (v === undefined) {
      delete next[key];
    } else {
      next[key] = v;
    }
    onChange(next as ScenarioDoc);
  };

  const simValues: Record<string, ConfigValue> = {};
  for (const o of SIM_OPTIONS) {
    const v = doc[o.key];
    if (typeof v === 'number') {
      simValues[o.key] = v;
    }
  }

  return (
    <div className="panel">
      <h3>Description</h3>
      <textarea
        rows={3}
        value={doc.description ?? ''}
        onChange={(e) => setTop('description', e.target.value || undefined)}
      />

      <h3>Simulation</h3>
      <SchemaForm
        options={SIM_OPTIONS}
        values={simValues}
        inherited={SCENARIO_DEFAULTS}
        onChange={setTop}
      />

      <h3>Channel (log-distance path loss)</h3>
      <SchemaForm
        options={PATH_LOSS_OPTIONS}
        values={without(doc.path_loss, 'model')}
        onChange={(k, v) => {
          const pl = setKey(doc.path_loss, k, v);
          onChange({ ...doc, path_loss: pl && { model: 'log_distance', ...pl } });
        }}
      />

      <h3>Radio of nodes without a device ({withoutDevice})</h3>
      <div className="row">
        <label>type</label>
        <select
          value={radioType}
          onChange={(e) => onChange({ ...doc, radio: { type: e.target.value } })}
        >
          {Object.keys(catalog.phys).map((p) => (
            <option key={p} value={p}>
              {p}
            </option>
          ))}
        </select>
      </div>
      <SchemaForm
        options={phyOptions}
        values={without(doc.radio, 'type')}
        onChange={(k, v) => {
          const r = setKey(doc.radio, k, v);
          // {"type": "lora"} alone is the default: leave "radio" out of the file
          const onlyDefault = radioType === 'lora' && without(r, 'type') === undefined;
          onChange({ ...doc, radio: onlyDefault ? undefined : r });
        }}
      />

      <h3>Nodes ({doc.nodes.length})</h3>
      <div className="node-list">
        {doc.nodes.map((n) => (
          <div
            key={n.id}
            className={n.id === selected ? 'selected' : undefined}
            onClick={() => onSelect(n.id)}
          >
            <span>{n.id}</span>
            <span>{n.device ?? <span className="hint">scenario radio</span>}</span>
            <span>{n.stack}</span>
          </div>
        ))}
      </div>
      <p className="hint">
        Click a node on the map to edit it, drag to move it, or use “Add node” and click the map.
      </p>
    </div>
  );
}
