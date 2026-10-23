/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import { describe, expect, it } from 'vitest';

import type { Hardware, ModuleInfo, PhyInfo } from '../api';
import {
  addNode,
  bounds,
  effectiveRadio,
  formatScenario,
  moveNode,
  newScenario,
  nextNodeId,
  nodeHardware,
  removeNode,
  setKey,
  stacksFor,
  type ScenarioDoc,
} from './scenario';

const phys: Record<string, PhyInfo> = {
  lora: {
    version: 1,
    options: [
      { key: 'tx_power_dbm', type: 'number', help: '', default: 14 },
      { key: 'sf', type: 'enum', help: '', values: [7, 8, 9], default: 7 },
    ],
  },
  nrf24: {
    version: 1,
    options: [
      { key: 'tx_power_dbm', type: 'number', help: '', default: 14 },
      { key: 'channel', type: 'int', help: '', default: 76 },
    ],
  },
};

const loraModule: ModuleInfo = {
  name: 'lora-mod',
  version: 1,
  title: 'LoRa module',
  description: '',
  chip: 'SX',
  radio: { type: 'lora', sf: 9, tx_power_dbm: 20 },
  limits: { tx_power_dbm: { min: 2, max: 20 } },
  antenna_dbi: 0,
  lna_db: 0,
  current_ma: { tx: 100, rx: 10 },
  pins: [],
  header: [],
  supply_v: null,
  notes: [],
};

const devices: Hardware = {
  boards: [
    {
      name: 'board',
      version: 1,
      title: 'Board',
      description: '',
      mcu: 'mcu1',
      module: 'lora-mod',
      integrated: true,
      radio: { type: 'lora', sf: 9, tx_power_dbm: 20 },
      limits: {},
    },
  ],
  mcus: [
    {
      name: 'mcu1',
      version: 1,
      title: 'MCU',
      description: '',
      chip: 'C',
      logic_v: 3.3,
      ram_kb: 2,
      flash_kb: 32,
      clock_mhz: 16,
      supply_out_ma: 50,
      header: null,
      io: [],
      input_only: [],
      spi: [],
      firmware: null,
      notes: [],
    },
  ],
  modules: [loraModule, { ...loraModule, name: 'nrf-mod', radio: { type: 'nrf24', channel: 9 } }],
};

describe('scenario editing', () => {
  it('adds nodes with fresh ids, copying the template but not its address', () => {
    let doc = newScenario();
    const template = { ...doc.nodes[0]!, config: { addr: 7, send_to: 2 } };
    doc = addNode(doc, 10, 20, template);
    const added = doc.nodes[2]!;
    expect(added.id).toBe(3);
    expect(added.pos).toEqual([10, 20]);
    expect(added.config).toEqual({ send_to: 2 });
    expect(template.config).toEqual({ addr: 7, send_to: 2 }); // template untouched
    expect(nextNodeId(doc)).toBe(4);
  });

  it('moves and removes nodes without touching the original', () => {
    const doc = newScenario();
    const moved = moveNode(doc, 2, 5, 6);
    expect(moved.nodes[1]!.pos).toEqual([5, 6]);
    expect(doc.nodes[1]!.pos).toEqual([800, 0]);
    expect(moveNode({ nodes: [{ id: 1, pos: [0, 0, 9], stack: 's' }] }, 1, 1, 2).nodes[0]!.pos)
      .toEqual([1, 2, 9]);
    expect(removeNode(doc, 1).nodes.map((n) => n.id)).toEqual([2]);
  });

  it('removes empty objects so saved files stay minimal', () => {
    expect(setKey({ a: 1 }, 'a', undefined)).toBeUndefined();
    expect(setKey(undefined, 'a', 2)).toEqual({ a: 2 });
    expect(setKey({ a: 1 }, 'b', true)).toEqual({ a: 1, b: true });
  });

  it('computes bounds', () => {
    expect(bounds(newScenario())).toEqual({ minX: 0, minY: 0, maxX: 800, maxY: 0 });
    expect(bounds({ nodes: [] }).maxX).toBe(500);
  });
});

describe('effective radio', () => {
  it('layers PHY defaults, device and node overrides', () => {
    const doc: ScenarioDoc = { nodes: [{ id: 1, pos: [0, 0], stack: 'k', device: 'board' }] };
    const r = effectiveRadio(doc, doc.nodes[0]!, devices, phys);
    expect(r.type).toBe('lora');
    expect(r.values).toMatchObject({ sf: 9, tx_power_dbm: 20 });

    const node = { ...doc.nodes[0]!, radio: { sf: 8 } };
    expect(effectiveRadio(doc, node, devices, phys).values.sf).toBe(8);
    expect(effectiveRadio(doc, node, devices, phys).inherited.sf).toBe(9);
  });

  it('uses the scenario radio for nodes without a device', () => {
    const doc: ScenarioDoc = { radio: { type: 'nrf24', channel: 5 }, nodes: [] };
    const r = effectiveRadio(doc, { id: 1, pos: [0, 0], stack: 'e' }, devices, phys);
    expect(r.type).toBe('nrf24');
    expect(r.values).toMatchObject({ channel: 5, tx_power_dbm: 0 });
    expect(effectiveRadio({ nodes: [] }, { id: 1, pos: [0, 0], stack: 'e' }, devices, phys).type)
      .toBe('lora');
  });

  it('resolves boards and MCU + module pairs', () => {
    const base = { id: 1, pos: [0, 0], stack: 's' };
    expect(nodeHardware({ ...base, device: 'board' }, devices)).toMatchObject({
      board: { name: 'board' },
      mcu: { name: 'mcu1' },
      module: { name: 'lora-mod' },
    });
    const pair = nodeHardware({ ...base, mcu: 'mcu1', module: 'nrf-mod' }, devices);
    expect(pair.board).toBeUndefined();
    expect(pair.radio).toEqual({ type: 'nrf24', channel: 9 });
    expect(nodeHardware({ ...base, module: 'nrf-mod' }, devices).mcu).toBeUndefined();
    expect(nodeHardware({ ...base, device: 'gone' }, devices).missing).toBe('gone');
    expect(nodeHardware({ ...base, mcu: 'gone', module: 'nrf-mod' }, devices).missing).toBe('gone');
    expect(nodeHardware({ ...base, module: 'gone' }, devices).missing).toBe('gone');
    expect(nodeHardware(base, devices)).toEqual({});

    const doc: ScenarioDoc = { nodes: [] };
    const r = effectiveRadio(doc, { ...base, mcu: 'mcu1', module: 'nrf-mod' }, devices, phys);
    expect(r.type).toBe('nrf24');
    expect(r.values.channel).toBe(9);
  });

  it('filters stacks by PHY', () => {
    const stacks = [
      { name: 'a', version: '1', schema: { phy: ['lora'], options: [] } },
      { name: 'b', version: '1', schema: { phy: ['nrf24'], options: [] } },
      { name: 'c', version: '1', schema: null },
    ];
    expect(stacksFor('lora', stacks).map((s) => s.name)).toEqual(['a', 'c']);
  });
});

describe('formatScenario', () => {
  it('writes one line per setting and per node and round-trips', () => {
    const doc = newScenario();
    const text = formatScenario(doc);
    expect(JSON.parse(text)).toEqual(doc);
    expect(text).toContain('    { "id": 1, "pos": [0, 0], "device": "esp32-sx1276"');
    expect(text.split('\n').length).toBe(Object.keys(doc).length + doc.nodes.length + 4);
    expect(text.endsWith('}\n')).toBe(true);
  });

  it('keeps unknown keys and skips undefined ones', () => {
    const doc = { nodes: [], extra: { a: [] }, description: undefined } as unknown as ScenarioDoc;
    expect(JSON.parse(formatScenario(doc))).toEqual({ nodes: [], extra: { a: [] } });
    expect(formatScenario({ nodes: [], x: {} } as ScenarioDoc)).toContain('"x": {}');
  });
});
