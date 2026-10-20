/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Number fields of option forms: range checks, and hex display/storage for options whose schema
// says "format": "hex" (radio addresses). Hex values are stored as strings like "0xE7E7E7E701",
// which every stack reads with strtoull(..., 0).

import type { OptionSpec } from '../api';
import type { ConfigValue } from './scenario';

export function outOfRange(o: OptionSpec, v: number): boolean {
  return (
    !Number.isFinite(v) ||
    (o.type === 'int' && !Number.isInteger(v)) ||
    (o.min !== undefined && v < o.min) ||
    (o.max !== undefined && v > o.max) ||
    (o.step !== undefined && o.min !== undefined && (v - o.min) % o.step !== 0)
  );
}

export function toHex(n: number): string {
  return `0x${n.toString(16).toUpperCase()}`;
}

// How a stored value appears in the field.
export function displayValue(o: OptionSpec, v: ConfigValue | undefined): string {
  if (v === undefined) {
    return '';
  }
  if (o.format === 'hex' && typeof v !== 'boolean') {
    const n = Number(v);
    return Number.isInteger(n) && n >= 0 ? toHex(n) : String(v);
  }
  return String(v);
}

// What the user typed -> the value to store. `valid: false` leaves the stored value unchanged.
// Hex fields accept "0x..." or decimal input and always store hex.
export function parseNumberInput(
  o: OptionSpec,
  text: string,
): { valid: boolean; value?: ConfigValue } {
  const t = text.trim();
  if (t === '') {
    return { valid: true, value: undefined };
  }
  const n = Number(t);
  if (outOfRange(o, n)) {
    return { valid: false };
  }
  return { valid: true, value: o.format === 'hex' ? toHex(n) : n };
}
