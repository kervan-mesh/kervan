/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import { describe, expect, it } from 'vitest';

import type { OptionSpec } from '../api';
import { displayValue, outOfRange, parseNumberInput, toHex } from './options';

const addr: OptionSpec = { key: 'addr', type: 'int', help: '', min: 0, max: 0xffffffffff, format: 'hex' };
const len: OptionSpec = { key: 'len', type: 'int', help: '', min: 0, max: 32 };
const ard: OptionSpec = { key: 'ard_us', type: 'int', help: '', min: 250, max: 4000, step: 250 };

describe('number options', () => {
  it('checks ranges, integers and steps', () => {
    expect(outOfRange(len, 32)).toBe(false);
    expect(outOfRange(len, 33)).toBe(true);
    expect(outOfRange(len, 1.5)).toBe(true);
    expect(outOfRange(len, Number.NaN)).toBe(true);
    expect(outOfRange(ard, 750)).toBe(false);
    expect(outOfRange(ard, 760)).toBe(true);
  });

  it('parses decimal input as numbers', () => {
    expect(parseNumberInput(len, ' 8 ')).toEqual({ valid: true, value: 8 });
    expect(parseNumberInput(len, '')).toEqual({ valid: true, value: undefined });
    expect(parseNumberInput(len, '99')).toEqual({ valid: false });
    expect(parseNumberInput(len, 'abc')).toEqual({ valid: false });
  });

  it('keeps hex options in hex, whatever was typed', () => {
    expect(parseNumberInput(addr, '0xe7e7e7e701')).toEqual({ valid: true, value: '0xE7E7E7E701' });
    expect(parseNumberInput(addr, '255')).toEqual({ valid: true, value: '0xFF' });
    expect(parseNumberInput(addr, '0x')).toEqual({ valid: false });
    expect(parseNumberInput(addr, '0x10000000000')).toEqual({ valid: false }); // over 5 bytes
    expect(toHex(0)).toBe('0x0');
  });

  it('shows stored values, also decimals saved earlier, as hex', () => {
    expect(displayValue(addr, '0xE7E7E7E701')).toBe('0xE7E7E7E701');
    expect(displayValue(addr, 996028180225)).toBe('0xE7E7E7E701');
    expect(displayValue(addr, 'garbage')).toBe('garbage');
    expect(displayValue(addr, undefined)).toBe('');
    expect(displayValue(len, 8)).toBe('8');
  });
});
