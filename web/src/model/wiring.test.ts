/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

import { describe, expect, it } from 'vitest';

import type { McuInfo, WiringRow } from '../api';
import { busOf, choosePin, headerPins, isGpio, optionsFor, wireColor } from './wiring';

// The Raspberry Pi Pico as sim/hardware/mcus/rpi-pico.json describes it.
const pico = {
  name: 'rpi-pico',
  header: {
    left: ['GP0', 'GP1', 'GND', 'GP2', 'GP3', 'GP4', 'GP5', 'GND', 'GP6', 'GP7', 'GP8', 'GP9', 'GND',
      'GP10', 'GP11', 'GP12', 'GP13', 'GND', 'GP14', 'GP15'],
    right: ['VBUS', 'VSYS', 'GND', '3V3_EN', '3V3(OUT)', 'ADC_VREF', 'GP28', 'AGND', 'GP27', 'GP26',
      'RUN', 'GP22', 'GND', 'GP21', 'GP20', 'GP19', 'GP18', 'GND', 'GP17', 'GP16'],
    top: 'USB',
  },
  spi: [
    { name: 'SPI0', sck: ['GP2', 'GP6', 'GP18', 'GP22'], mosi: ['GP3', 'GP7', 'GP19'], miso: ['GP0', 'GP4', 'GP16'] },
    { name: 'SPI1', sck: ['GP10', 'GP14', 'GP26'], mosi: ['GP11', 'GP15', 'GP27'], miso: ['GP8', 'GP12', 'GP28'] },
  ],
} as unknown as McuInfo;

const row = (module_pin: string, role: string, mcu_name: string): WiringRow => ({
  module_pin,
  role,
  mcu_name,
  mcu_pin: mcu_name,
  mcu_number: 0,
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
  it('numbers the header like the board', () => {
    const pins = headerPins(pico);
    expect(pins).toHaveLength(40);
    expect(pins.find((p) => p.name === 'GP0')).toMatchObject({ number: 1, side: 'left', index: 0 });
    expect(pins.find((p) => p.name === 'VBUS')!.number).toBe(40);
    expect(pins.find((p) => p.name === '3V3(OUT)')!.number).toBe(36);
    expect(pins.find((p) => p.name === 'GP16')!.number).toBe(21);
    expect(headerPins({ ...pico, header: null })).toEqual([]);
  });

  it('offers the pins a signal can use', () => {
    expect(optionsFor(pico, 'spi_sck')).toEqual(['GP2', 'GP6', 'GP10', 'GP14', 'GP18', 'GP22', 'GP26']);
    const gpios = optionsFor(pico, 'gpio_out');
    expect(gpios).toHaveLength(26); // GP0..GP22 and GP26..GP28 (GP23-25 are inside the board)
    expect(gpios[0]).toBe('GP0');
    expect(gpios.every(isGpio)).toBe(true);
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
