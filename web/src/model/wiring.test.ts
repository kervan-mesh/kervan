/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import { describe, expect, it } from 'vitest';

import type { McuInfo, WiringRow } from '../api';
import { busOf, choosePin, isGpio, layoutHeader, optionsFor, pinTitle, wireColor } from './wiring';

// The Raspberry Pi Pico as sim/hardware/mcus/rpi-pico.json describes it.
const pin = (name: string, number: number) => ({ name, label: name, number });
const left = ['GP0', 'GP1', 'GND', 'GP2', 'GP3', 'GP4', 'GP5', 'GND', 'GP6', 'GP7', 'GP8', 'GP9', 'GND',
  'GP10', 'GP11', 'GP12', 'GP13', 'GND', 'GP14', 'GP15'];
const right = ['VBUS', 'VSYS', 'GND', '3V3_EN', '3V3(OUT)', 'ADC_VREF', 'GP28', 'AGND', 'GP27', 'GP26',
  'RUN', 'GP22', 'GND', 'GP21', 'GP20', 'GP19', 'GP18', 'GND', 'GP17', 'GP16'];
const pico = {
  name: 'rpi-pico',
  header: {
    top: 'USB',
    bottom: '',
    strips: [
      { name: '', side: 'left', gap: 0, pins: left.map((n, i) => pin(n, i + 1)) },
      { name: '', side: 'right', gap: 0, pins: right.map((n, i) => pin(n, 40 - i)) },
    ],
  },
  io: [...Array.from({ length: 23 }, (_, i) => `GP${i}`), 'GP26', 'GP27', 'GP28'],
  input_only: [],
  spi: [
    { name: 'SPI0', sck: ['GP2', 'GP6', 'GP18', 'GP22'], mosi: ['GP3', 'GP7', 'GP19'], miso: ['GP0', 'GP4', 'GP16'] },
    { name: 'SPI1', sck: ['GP10', 'GP14', 'GP26'], mosi: ['GP11', 'GP15', 'GP27'], miso: ['GP8', 'GP12', 'GP28'] },
  ],
} as unknown as McuInfo;

// An Arduino-style board: labelled pins, connectors with a gap, an input-only pin, a bottom row.
const board = {
  name: 'board',
  header: {
    top: 'USB',
    bottom: '',
    strips: [
      { name: 'CN1', side: 'left', gap: 0, pins: [pin('3V3', 1), pin('GND', 2)] },
      { name: 'CN2', side: 'left', gap: 1, pins: [{ name: 'PA5', label: 'D13', number: 0 }] },
      { name: '', side: 'bottom', gap: 2, pins: [pin('PB1', 0), pin('PB2', 0)] },
    ],
  },
  io: ['PA5', 'PB1', 'PB2'],
  input_only: ['PB2'],
  spi: [],
} as unknown as McuInfo;

const row = (module_pin: string, role: string, mcu_name: string): WiringRow => ({
  module_pin,
  role,
  mcu_name,
  mcu_pin: mcu_name,
  mcu_number: 0,
  mcu_strip: -1,
  mcu_index: -1,
});

// the default wiring ksim reports for the Pico and an nRF24 module
const rows: WiringRow[] = [
  row('VCC', 'power', '3V3(OUT)'),
  row('GND', 'gnd', 'GND'),
  row('CE', 'gpio_out', 'GP20'),
  row('CSN', 'spi_cs', 'GP17'),
  row('SCK', 'spi_sck', 'GP18'),
  row('MOSI', 'spi_mosi', 'GP19'),
  row('MISO', 'spi_miso', 'GP16'),
  row('IRQ', 'gpio_in_optional', 'GP21'),
];

describe('wiring', () => {
  it('lays out the header like the board', () => {
    const l = layoutHeader(pico);
    expect(l.pins).toHaveLength(40);
    expect(l.rows).toBe(20);
    expect(l.pins.find((p) => p.name === 'GP0')).toMatchObject({ number: 1, side: 'left', row: 0, strip: 0 });
    expect(l.pins.find((p) => p.name === 'VBUS')).toMatchObject({ number: 40, side: 'right', row: 0 });
    expect(l.pins.find((p) => p.name === 'GP16')).toMatchObject({ number: 21, row: 19, index: 19 });
    expect(layoutHeader({ ...pico, header: null }).pins).toEqual([]);

    const b = layoutHeader(board);
    expect(b.rows).toBe(4); // 2 pins, a gap, 1 pin
    expect(b.pins.find((p) => p.name === 'PA5')).toMatchObject({ row: 3, strip: 1, index: 0, label: 'D13' });
    expect(b.bottomRows).toBe(1);
    expect(b.bottomCols).toBe(4);
    expect(b.pins.find((p) => p.name === 'PB2')).toMatchObject({ side: 'bottom', row: 0, col: 3 });
    expect(b.strips.map((s) => s.name)).toEqual(['CN1', 'CN2', '']);
  });

  it('names pins as the board prints them', () => {
    expect(pinTitle(pico, 'GP15')).toBe('GP15 · pin 20');
    expect(pinTitle(board, 'PA5')).toBe('D13 = PA5 · CN2');
    expect(pinTitle(board, '3V3')).toBe('3V3 · CN1 pin 1');
    expect(pinTitle(board, 'XX')).toBe('XX');
  });

  it('offers the pins a signal can use', () => {
    expect(optionsFor(pico, 'spi_sck')).toEqual(['GP2', 'GP6', 'GP10', 'GP14', 'GP18', 'GP22', 'GP26']);
    const gpios = optionsFor(pico, 'gpio_out');
    expect(gpios).toHaveLength(26); // GP0..GP22 and GP26..GP28 (GP23-25 are inside the board)
    expect(gpios[0]).toBe('GP0');
    expect(gpios.every((g) => isGpio(pico, g))).toBe(true);
    expect(isGpio(pico, 'GND')).toBe(false);
    // input-only pins: fine for inputs, not for signals the MCU drives
    expect(optionsFor(board, 'gpio_in')).toEqual(['PA5', 'PB1', 'PB2']);
    expect(optionsFor(board, 'gpio_out')).toEqual(['PA5', 'PB1']);
    expect(optionsFor(board, 'spi_sck')).toEqual(['PA5', 'PB1']); // no SPI list: any GPIO
    expect(busOf(pico, 'spi_miso', 'GP12')).toBe('SPI1');
    expect(busOf(pico, 'spi_cs', 'GP17')).toBeUndefined();
  });

  it('swaps a pin that is already in use', () => {
    expect(choosePin(pico, rows, {}, 'CE', 'GP17')).toEqual({ CE: 'GP17', CSN: 'GP20' });
    expect(choosePin(pico, rows, {}, 'CE', 'GP15')).toEqual({ CE: 'GP15' });
  });

  it('keeps SCK, MOSI and MISO on one controller', () => {
    // SCK onto SPI1: MOSI and MISO move to free SPI1 pins
    expect(choosePin(pico, rows, {}, 'SCK', 'GP10')).toEqual({ SCK: 'GP10', MOSI: 'GP11', MISO: 'GP8' });
    // within the same controller nothing else moves
    expect(choosePin(pico, rows, {}, 'SCK', 'GP2')).toEqual({ SCK: 'GP2' });
    // a free pin of the new controller, not one taken by CE
    const ceOn11 = choosePin(pico, rows, { CE: 'GP11' }, 'SCK', 'GP14');
    expect(ceOn11.MOSI).toBe('GP15');
  });

  it('colours wires by signal', () => {
    expect(wireColor('power')).toBe('#e5534b');
    expect(wireColor('something')).toBe('#8c959f');
  });
});
