/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Choosing which MCU pin each module pin goes to (docs/hardware.md, "Wiring and firmware").
// ksim checks a choice (POST /api/wiring); these helpers offer only sensible pins, lay out the
// header for drawing, and keep a choice consistent while the user picks: a pin already in use
// swaps places, and the SPI signals follow each other onto one SPI controller.

import type { McuInfo, WiringRow } from '../api';

export type PinChoice = Record<string, string>; // module pin -> MCU pin, e.g. { CE: 'GP15' }

export interface PlacedPin {
  name: string; // the MCU's name: "PA5"
  label: string; // printed on the board: "D13"
  number: number; // on its connector, 0 if not numbered
  side: 'left' | 'right' | 'bottom';
  strip: number; // index into header.strips
  index: number; // within the strip
  row: number; // left/right: position down the edge (gaps included); bottom: which bottom row
  col: number; // bottom: position along the row; left/right: 0
}

export interface HeaderLayout {
  pins: PlacedPin[];
  rows: number; // positions down the left and right edges
  bottomRows: number;
  bottomCols: number;
  strips: { name: string; side: string; row: number; col: number }[]; // where each strip starts
}

// Positions of every header pin, for drawing.
export function layoutHeader(mcu: McuInfo): HeaderLayout {
  const out: HeaderLayout = { pins: [], rows: 0, bottomRows: 0, bottomCols: 0, strips: [] };
  const h = mcu.header;
  if (!h) {
    return out;
  }
  const down = { left: 0, right: 0 };
  h.strips.forEach((s, k) => {
    if (s.side === 'bottom') {
      const row = out.bottomRows++;
      out.strips.push({ name: s.name, side: s.side, row, col: s.gap });
      s.pins.forEach((p, i) => out.pins.push({ ...p, side: 'bottom', strip: k, index: i, row, col: s.gap + i }));
      out.bottomCols = Math.max(out.bottomCols, s.gap + s.pins.length);
    } else {
      const side = s.side;
      down[side] += s.gap;
      out.strips.push({ name: s.name, side, row: down[side], col: 0 });
      s.pins.forEach((p, i) => out.pins.push({ ...p, side, strip: k, index: i, row: down[side] + i, col: 0 }));
      down[side] += s.pins.length;
    }
  });
  out.rows = Math.max(down.left, down.right);
  return out;
}

export function isGpio(mcu: McuInfo, name: string): boolean {
  return mcu.io.includes(name);
}

const SPI_ROLES = ['spi_sck', 'spi_mosi', 'spi_miso'] as const;
type SpiRole = (typeof SPI_ROLES)[number];
const SPI_KEY: Record<SpiRole, 'sck' | 'mosi' | 'miso'> = {
  spi_sck: 'sck',
  spi_mosi: 'mosi',
  spi_miso: 'miso',
};

function isSpiRole(role: string): role is SpiRole {
  return (SPI_ROLES as readonly string[]).includes(role);
}

// Signals that can move (not supply pins).
export function movable(role: string): boolean {
  return role !== 'power' && role !== 'gnd';
}

// Signals the MCU drives: not on input-only pins.
function drives(role: string): boolean {
  return role === 'gpio_out' || role === 'spi_cs' || role === 'spi_sck' || role === 'spi_mosi';
}

// The MCU pins a module pin of this role can go to, in the catalog's GPIO order.
export function optionsFor(mcu: McuInfo, role: string): string[] {
  let names = mcu.io;
  if (isSpiRole(role) && mcu.spi.length > 0) {
    const allowed = new Set(mcu.spi.flatMap((b) => b[SPI_KEY[role]]));
    names = mcu.io.filter((n) => allowed.has(n));
  }
  if (drives(role)) {
    names = names.filter((n) => !mcu.input_only.includes(n));
  }
  return names;
}

// The SPI controller a pin belongs to for a role, e.g. "SPI1".
export function busOf(mcu: McuInfo, role: string, name: string): string | undefined {
  if (!isSpiRole(role)) {
    return undefined;
  }
  return mcu.spi.find((b) => b[SPI_KEY[role]].includes(name))?.name;
}

// "PA5 (D13)", or "GP15" when the board prints the same name.
export function pinTitle(mcu: McuInfo, name: string): string {
  for (const s of mcu.header?.strips ?? []) {
    const p = s.pins.find((x) => x.name === name);
    if (p) {
      const where = p.number > 0 ? ` · ${s.name ? `${s.name} ` : ''}pin ${p.number}` : s.name ? ` · ${s.name}` : '';
      return `${p.label !== p.name ? `${p.label} = ${p.name}` : p.name}${where}`;
    }
  }
  return name;
}

// Put `modulePin` on `target`, starting from the current wiring `rows` and the choice so far.
export function choosePin(
  mcu: McuInfo,
  rows: WiringRow[],
  choice: PinChoice,
  modulePin: string,
  target: string,
): PinChoice {
  const current = new Map(rows.map((r) => [r.module_pin, r.mcu_name]));
  for (const [pin, to] of Object.entries(choice)) {
    current.set(pin, to); // rows may lag behind the choice
  }
  const role = new Map(rows.map((r) => [r.module_pin, r.role]));
  const next: PinChoice = { ...choice, [modulePin]: target };
  const before = current.get(modulePin) ?? '';
  current.set(modulePin, target);

  // the pin that had `target` gets this pin's old one (if it may)
  for (const r of rows) {
    if (r.module_pin !== modulePin && movable(r.role) && current.get(r.module_pin) === target) {
      if (before && optionsFor(mcu, r.role).includes(before)) {
        next[r.module_pin] = before;
        current.set(r.module_pin, before);
      }
    }
  }

  // the other SPI signals follow onto the same controller
  const myRole = role.get(modulePin) ?? '';
  const bus = mcu.spi.find((b) => b.name === busOf(mcu, myRole, target));
  if (bus) {
    for (const r of rows) {
      if (r.module_pin === modulePin || !isSpiRole(r.role)) {
        continue;
      }
      if (!bus[SPI_KEY[r.role]].includes(current.get(r.module_pin) ?? '')) {
        const taken = new Set([...current.values()]);
        const free = optionsFor(mcu, r.role).find((p) => bus[SPI_KEY[r.role as SpiRole]].includes(p) && !taken.has(p));
        if (free) {
          next[r.module_pin] = free;
          current.set(r.module_pin, free);
        }
      }
    }
  }
  return next;
}

// Wire colours by signal, readable on light and dark backgrounds.
export const WIRE_COLORS: Record<string, string> = {
  power: '#e5534b',
  gnd: '#768390',
  spi_sck: '#d4a72c',
  spi_mosi: '#2f81f7',
  spi_miso: '#2da44e',
  spi_cs: '#e16f24',
  gpio_out: '#a371f7',
  gpio_in: '#db61a2',
  gpio_in_optional: '#8c959f',
};

export function wireColor(role: string): string {
  return WIRE_COLORS[role] ?? '#8c959f';
}
