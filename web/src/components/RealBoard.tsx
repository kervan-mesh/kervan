/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// A real board for a node (docs/hardware.md): which serial port it is on, and whether the Kervan
// firmware answers there. The board joins ● Live sessions next to the simulated node.

import { useCallback, useEffect, useState } from 'react';

import { boardHello, listPorts, wiringFor, type BoardHello } from '../api';
import { withField, type NodeDoc } from '../model/scenario';

interface Props {
  node: NodeDoc;
  usable: boolean; // nrf24-esb on an nRF24 radio
  parts?: { mcu: string; module: string }; // to compare the board's firmware with the wiring
  onChange: (n: NodeDoc) => void;
}

export function RealBoard({ node, usable, parts, onChange }: Props) {
  const [ports, setPorts] = useState<string[] | null>(null);
  const [hello, setHello] = useState<BoardHello | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [checking, setChecking] = useState(false);
  const [expected, setExpected] = useState<string | null>(null); // the wiring the node asks for
  const port = node.hardware?.port ?? '';

  const refresh = useCallback(async () => {
    try {
      setPorts((await listPorts()).ports.map((p) => p.port));
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  useEffect(() => {
    setHello(null);
    setError(null);
  }, [port]);

  const check = async () => {
    setChecking(true);
    setHello(null);
    setError(null);
    try {
      const h = (await boardHello(port)).hello;
      setHello(h);
      setExpected(null);
      if (h.wiring && parts) {
        const w = await wiringFor(parts.mcu, parts.module, node.pins ?? {});
        setExpected(w.firmware?.wiring ?? null);
      }
    } catch (e) {
      setError(e instanceof Error ? e.message : String(e));
    } finally {
      setChecking(false);
    }
  };

  const choices = ports ?? [];
  return (
    <div>
      <div className="row">
        <label>serial port</label>
        <div className="inline">
          <select
            value={port}
            onChange={(e) =>
              onChange(withField(node, 'hardware', e.target.value ? { port: e.target.value } : undefined))
            }
          >
            <option value="">none (simulation only)</option>
            {port && !choices.includes(port) && <option value={port}>{port} (not connected)</option>}
            {choices.map((p) => (
              <option key={p} value={p}>
                {p}
              </option>
            ))}
          </select>
          <button style={{ flex: 'none' }} onClick={() => void refresh()} title="Look for ports again">
            ↻
          </button>
          <button
            style={{ flex: 'none' }}
            onClick={() => void check()}
            disabled={!port || checking}
            title="Ask the board on this port what it is"
          >
            {checking ? '…' : 'Check'}
          </button>
        </div>
        <div className="help">
          {ports !== null && ports.length === 0
            ? 'No serial ports found: plug in a board with the Kervan firmware.'
            : 'The board runs this node too during ● Live sessions, next to its simulation.'}
        </div>
      </div>
      {port && !usable && (
        <p className="warning">⚠ Real boards run the nrf24-esb stack on an nRF24 module.</p>
      )}
      {hello && (
        <p className={hello.radio === 'none' ? 'warning' : 'hint'}>
          {hello.radio === 'none' ? '⚠ ' : '✓ '}
          {hello.board} {hello.id} · {hello.fw} v{hello.version} ·{' '}
          {hello.radio === 'none' ? 'no nRF24 radio found: check the wiring' : `radio ${hello.radio}`}
          {hello.wiring && (
            <>
              <br />
              firmware pins: <code>{hello.wiring}</code>
            </>
          )}
        </p>
      )}
      {hello?.wiring && expected && expected !== hello.wiring && (
        <p className="warning">
          ⚠ This node is wired <code>{expected}</code>: build and flash the firmware for it
          (Hardware → Wiring &amp; firmware), or wire the module as the board expects.
        </p>
      )}
      {error && (
        <p className="hint" style={{ color: 'var(--danger)' }}>
          {error}
        </p>
      )}
    </div>
  );
}
