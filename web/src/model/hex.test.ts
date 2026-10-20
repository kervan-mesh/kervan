/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import { describe, expect, it } from 'vitest';

import { asciiChar, byteHex, bytesFromHex, dumpRows, parseHexInput } from './hex';

describe('hex', () => {
  it('parses payload input like the scenario parser', () => {
    expect(parseHexInput('01 A2 ff').bytes).toEqual([1, 0xa2, 0xff]);
    expect(parseHexInput(' 0x01a2\nFF ').bytes).toEqual([1, 0xa2, 0xff]);
    expect(parseHexInput('').bytes).toEqual([]);
    expect(parseHexInput('012')).toEqual({ bytes: null, error: 'two hex digits per byte' });
    expect(parseHexInput('01 zz').error).toContain('“z”');
    expect(parseHexInput('aa'.repeat(1025)).error).toContain('1024');
  });

  it('formats dumps', () => {
    expect(bytesFromHex('0a0bff')).toEqual([10, 11, 255]);
    expect(byteHex(10)).toBe('0A');
    expect(dumpRows([1, 2, 3, 4, 5], 2)).toEqual([
      { offset: 0, bytes: [1, 2] },
      { offset: 2, bytes: [3, 4] },
      { offset: 4, bytes: [5] },
    ]);
    expect(asciiChar(0x41)).toBe('A');
    expect(asciiChar(0)).toBe('·');
  });
});
