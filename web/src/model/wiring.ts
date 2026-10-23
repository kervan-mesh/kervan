/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Choosing which MCU pin each module pin goes to (docs/hardware.md, "Wiring and firmware").
// ksim checks a choice (POST /api/wiring); these helpers offer only sensible pins and keep a
// choice consistent while the user picks: a pin already in use swaps places, and the SPI signals
// follow each other onto one SPI controller.

import type { McuInfo, WiringRow } from '../api';

export type PinChoice = Record<string, string>; // module pin -> MCU pin, e.g. { CE: 'GP15' }

export interface HeaderPin {
  name: string;
  number: number; // physical pin
  side: 'left' | 'right';
  index: number; // 0 = top
}

// Every pin of an MCU's header, in drawing order.
export function headerPins(mcu: McuInfo): HeaderPin[] {
  const h = mcu.header;
  if (!h) {
    return [];
  }
  const n = h.left.length;
  return [
    ...h.left.map((name, i) => ({ name, number: i + 1, side: 'left' as const, index: i })),
    ...h.right.map((name, i) => ({ name, number: 2 * n - i, side: 'right' as const, index: i })),
  ];
}

export function isGpio(name: string): boolean {
  return /^GP\d+$/.test(name);
}

export function gpioNumber(name: string): number {
  return isGpio(name) ? Number(name.slice(2)) : -1;
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

// The MCU pins a module pin of this role can go to, in GPIO order.
export function optionsFor(mcu: McuInfo, role: string): string[] {
  let names: string[];
  if (isSpiRole(role) && mcu.spi.length > 0) {
    names = mcu.spi.flatMap((b) => b[SPI_KEY[role]]);
  } else {
    names = headerPins(mcu)
      .map((p) => p.name)
      .filter(isGpio);
  }
  return [...new Set(names)].sort((a, b) => gpioNumber(a) - gpioNumber(b));
}

// The SPI controller a pin belongs to for a role, e.g. "SPI1".
export function busOf(mcu: McuInfo, role: string, name: string): string | undefined {
  if (!isSpiRole(role)) {
    return undefined;
  }
  return mcu.spi.find((b) => b[SPI_KEY[role]].includes(name))?.name;
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

  // the pin that had `target` gets this pin's old one
  for (const r of rows) {
    if (r.module_pin !== modulePin && movable(r.role) && current.get(r.module_pin) === target) {
      if (before) {
        next[r.module_pin] = before;
        current.set(r.module_pin, before);
      }
    }
  }

  // the other SPI signals follow onto the same controller
  const myRole = role.get(modulePin) ?? '';
  const bus = mcu.spi.find((b) => b.name === busOf(mcu, myRole, target));
  if (bus) {
    const used = () => new Set([...current.values()]);
    for (const r of rows) {
      if (r.module_pin === modulePin || !isSpiRole(r.role)) {
        continue;
      }
      const list = bus[SPI_KEY[r.role]];
      if (!list.includes(current.get(r.module_pin) ?? '')) {
        const taken = used();
        const free = list.find((p) => !taken.has(p));
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
