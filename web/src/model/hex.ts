/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Hex text for payload input and the packet inspector's hex dump.

export interface HexInput {
  bytes: number[] | null; // null when the text is not valid
  error?: string;
}

// Same rules as the scenario parser: optional 0x, whitespace anywhere, two digits per byte.
export function parseHexInput(text: string): HexInput {
  let t = text.trim();
  if (/^0x/i.test(t)) {
    t = t.slice(2);
  }
  const digits = t.replace(/\s+/g, '');
  const bad = digits.match(/[^0-9a-fA-F]/);
  if (bad) {
    return { bytes: null, error: `“${bad[0]}” is not a hex digit` };
  }
  if (digits.length % 2 !== 0) {
    return { bytes: null, error: 'two hex digits per byte' };
  }
  if (digits.length > 2048) {
    return { bytes: null, error: 'longer than 1024 bytes' };
  }
  const bytes: number[] = [];
  for (let i = 0; i < digits.length; i += 2) {
    bytes.push(parseInt(digits.slice(i, i + 2), 16));
  }
  return { bytes };
}

export function bytesFromHex(hex: string): number[] {
  const out: number[] = [];
  for (let i = 0; i + 1 < hex.length; i += 2) {
    out.push(parseInt(hex.slice(i, i + 2), 16));
  }
  return out;
}

export function byteHex(b: number): string {
  return b.toString(16).padStart(2, '0').toUpperCase();
}

export interface DumpRow {
  offset: number;
  bytes: number[];
}

export function dumpRows(bytes: number[], width = 8): DumpRow[] {
  const rows: DumpRow[] = [];
  for (let i = 0; i < bytes.length; i += width) {
    rows.push({ offset: i, bytes: bytes.slice(i, i + width) });
  }
  return rows;
}

// Printable ASCII or '.' (for the text column of the dump).
export function asciiChar(b: number): string {
  return b >= 0x20 && b < 0x7f ? String.fromCharCode(b) : '·';
}

// Text as UTF-8 bytes in the payload editor's hex format ("4D 65 72").
export function textToHex(text: string): string {
  return [...new TextEncoder().encode(text)].map(byteHex).join(' ');
}

// The bytes as text if they are valid UTF-8 without control characters (tab and newlines
// allowed), else null.
export function bytesToText(bytes: number[]): string | null {
  try {
    const text = new TextDecoder('utf-8', { fatal: true }).decode(new Uint8Array(bytes));
    return /[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F]/.test(text) ? null : text;
  } catch {
    return null;
  }
}
