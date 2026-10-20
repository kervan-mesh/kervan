/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// The scenario file (docs/simulator.md) as the editor holds it: the parsed JSON itself, so keys
// the editor does not know (e.g. "process") survive a round trip. All updates return new objects.

import type { DeviceInfo, PhyInfo, RadioValues, StackInfo } from '../api';

export type ConfigValue = number | string | boolean;

// What a node sends instead of its stack's test data: exactly one key.
export interface PayloadDoc {
  hex?: string;
  python?: string;
}

export interface NodeDoc {
  id: number;
  pos: number[]; // [x, y] or [x, y, z] in meters
  stack: string;
  device?: string;
  config?: Record<string, ConfigValue>;
  radio?: Record<string, ConfigValue>;
  payload?: PayloadDoc;
  [other: string]: unknown;
}

export interface ScenarioDoc {
  description?: string;
  seed?: number;
  duration_s?: number;
  mtu?: number;
  radio?: Record<string, ConfigValue>;
  path_loss?: Record<string, ConfigValue>;
  capture_threshold_db?: number;
  noise_figure_db?: number;
  duty_cycle_limit?: number;
  nodes: NodeDoc[];
  [other: string]: unknown;
}

// Simulator defaults for top-level keys (sim/include/ksim/simulator.hpp).
export const SCENARIO_DEFAULTS = {
  seed: 1,
  duration_s: 60,
  mtu: 255,
  capture_threshold_db: 6,
  noise_figure_db: 6,
  duty_cycle_limit: 0.01,
};

export function newScenario(): ScenarioDoc {
  return {
    description: 'New scenario',
    seed: 1,
    duration_s: 600,
    mtu: 51,
    path_loss: { model: 'log_distance', exponent: 3, shadowing_sigma_db: 4 },
    nodes: [
      {
        id: 1,
        pos: [0, 0],
        device: 'esp32-sx1276',
        stack: 'kervan',
        config: { send_to: 2, send_every_ms: 30000, qos: 1 },
      },
      { id: 2, pos: [800, 0], device: 'esp32-sx1276', stack: 'kervan' },
    ],
  };
}

export function findNode(doc: ScenarioDoc, id: number): NodeDoc | undefined {
  return doc.nodes.find((n) => n.id === id);
}

export function nextNodeId(doc: ScenarioDoc): number {
  return doc.nodes.reduce((max, n) => Math.max(max, n.id), 0) + 1;
}

export function updateNode(doc: ScenarioDoc, id: number, f: (n: NodeDoc) => NodeDoc): ScenarioDoc {
  return { ...doc, nodes: doc.nodes.map((n) => (n.id === id ? f(n) : n)) };
}

export function moveNode(doc: ScenarioDoc, id: number, x: number, y: number): ScenarioDoc {
  return updateNode(doc, id, (n) => ({
    ...n,
    pos: n.pos.length > 2 ? [x, y, n.pos[2] ?? 0] : [x, y],
  }));
}

// A new node at (x, y) like `template` (same device, stack and settings, no traffic target).
export function addNode(doc: ScenarioDoc, x: number, y: number, template?: NodeDoc): ScenarioDoc {
  const id = nextNodeId(doc);
  const node: NodeDoc = template
    ? (JSON.parse(JSON.stringify(template)) as NodeDoc)
    : { id, pos: [x, y], stack: 'kervan' };
  node.id = id;
  node.pos = [x, y];
  if (node.config && 'addr' in node.config) {
    delete node.config.addr; // addresses default to the node id
  }
  return { ...doc, nodes: [...doc.nodes, node] };
}

export function removeNode(doc: ScenarioDoc, id: number): ScenarioDoc {
  return { ...doc, nodes: doc.nodes.filter((n) => n.id !== id) };
}

// Set or (with undefined) remove one key of an object-valued field such as config or radio.
export function setKey<T extends Record<string, ConfigValue>>(
  obj: T | undefined,
  key: string,
  value: ConfigValue | undefined,
): T | undefined {
  const next = { ...(obj ?? {}) } as Record<string, ConfigValue>;
  if (value === undefined) {
    delete next[key];
  } else {
    next[key] = value;
  }
  return Object.keys(next).length > 0 ? (next as T) : undefined;
}

// Replace a node field; undefined removes it so files stay minimal.
export function withField<K extends keyof NodeDoc>(n: NodeDoc, key: K, value: NodeDoc[K]): NodeDoc {
  const next = { ...n };
  if (value === undefined) {
    delete next[key];
  } else {
    next[key] = value;
  }
  return next;
}

export interface EffectiveRadio {
  type: string;
  values: RadioValues; // every setting of the PHY
  inherited: RadioValues; // what the node gets without its own "radio" overrides
}

// The radio a node ends up with, resolved like the simulator does: PHY defaults, then the device
// (or the scenario radio), then the node's overrides.
export function effectiveRadio(
  doc: ScenarioDoc,
  node: NodeDoc,
  devices: DeviceInfo[],
  phys: Record<string, PhyInfo>,
): EffectiveRadio {
  const device = node.device ? devices.find((d) => d.name === node.device) : undefined;
  const base: Record<string, ConfigValue> = device ? device.radio : (doc.radio ?? {});
  let type = typeof base.type === 'string' ? base.type : 'lora';
  if (typeof node.radio?.type === 'string') {
    type = node.radio.type;
  }
  const defaults: RadioValues = { type };
  for (const o of phys[type]?.options ?? []) {
    if (o.default !== undefined) {
      defaults[o.key] = o.default;
    }
  }
  if (type === 'nrf24') {
    defaults.tx_power_dbm = 0; // as the scenario parser does when switching PHY
  }
  const sameType = (base.type ?? 'lora') === type;
  const inherited: RadioValues = { ...defaults, ...(sameType ? base : {}), type };
  const values: RadioValues = { ...inherited, ...(node.radio ?? {}), type };
  return { type, values, inherited };
}

export function stacksFor(phy: string, stacks: StackInfo[]): StackInfo[] {
  return stacks.filter((s) => s.schema === null || s.schema.phy.includes(phy));
}

export interface Bounds {
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
}

export function bounds(doc: ScenarioDoc): Bounds {
  if (doc.nodes.length === 0) {
    return { minX: -500, minY: -500, maxX: 500, maxY: 500 };
  }
  const xs = doc.nodes.map((n) => n.pos[0] ?? 0);
  const ys = doc.nodes.map((n) => n.pos[1] ?? 0);
  return {
    minX: Math.min(...xs),
    minY: Math.min(...ys),
    maxX: Math.max(...xs),
    maxY: Math.max(...ys),
  };
}

// JSON on one line, with a space after ':' and ',' like the hand-written scenario files.
function inline(v: unknown): string {
  if (Array.isArray(v)) {
    return `[${v.map(inline).join(', ')}]`;
  }
  if (v !== null && typeof v === 'object') {
    const items = Object.entries(v as Record<string, unknown>).map(
      ([k, x]) => `${JSON.stringify(k)}: ${inline(x)}`,
    );
    return items.length > 0 ? `{ ${items.join(', ')} }` : '{}';
  }
  return JSON.stringify(v);
}

// Scenario files keep one line per setting and per node, so diffs stay readable.
export function formatScenario(doc: ScenarioDoc): string {
  const lines: string[] = [];
  const entries = Object.entries(doc).filter(([, v]) => v !== undefined);
  entries.forEach(([key, value], i) => {
    const comma = i < entries.length - 1 ? ',' : '';
    if (key === 'nodes' && Array.isArray(value)) {
      lines.push(`  "nodes": [`);
      value.forEach((n, j) => lines.push(`    ${inline(n)}${j < value.length - 1 ? ',' : ''}`));
      lines.push(`  ]${comma}`);
    } else {
      lines.push(`  ${JSON.stringify(key)}: ${inline(value)}${comma}`);
    }
  });
  return `{\n${lines.join('\n')}\n}\n`;
}

// obj without `key` (e.g. a radio object without its "type"), or undefined if nothing is left.
export function without(
  obj: Record<string, ConfigValue> | undefined,
  key: string,
): Record<string, ConfigValue> | undefined {
  return setKey(obj, key, undefined);
}
