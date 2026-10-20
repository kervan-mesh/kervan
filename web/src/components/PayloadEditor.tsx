/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// What a node sends: the stack's own test data, fixed bytes, or a Python function
// ("payload" in the scenario, docs/simulator.md).

import { useState, type KeyboardEvent } from 'react';

import { parseHexInput } from '../model/hex';
import type { PayloadDoc } from '../model/scenario';

type Mode = 'stack' | 'hex' | 'python';

export const PYTHON_TEMPLATE = `def payload(node, seq, t):
    # node: this node's id, seq: message number (0, 1, ...), t: virtual time in seconds
    # rng: a seeded random.Random; struct, math and random are imported
    temperature = 20.0 + 5.0 * math.sin(t / 600) + rng.uniform(-0.5, 0.5)
    return struct.pack('<BHf', node, seq & 0xFFFF, temperature)
`;

interface Props {
  payload: PayloadDoc | undefined;
  pythonAllowed: boolean; // ksim serve --allow-processes
  onChange: (p: PayloadDoc | undefined) => void;
}

function modeOf(p: PayloadDoc | undefined): Mode {
  return p?.python !== undefined ? 'python' : p?.hex !== undefined ? 'hex' : 'stack';
}

// Tab inserts four spaces instead of leaving the code editor.
function onCodeKey(e: KeyboardEvent<HTMLTextAreaElement>, set: (v: string) => void) {
  if (e.key !== 'Tab' || e.shiftKey) {
    return;
  }
  e.preventDefault();
  const el = e.currentTarget;
  const { selectionStart: s, selectionEnd: end, value } = el;
  set(value.slice(0, s) + '    ' + value.slice(end));
  requestAnimationFrame(() => el.setSelectionRange(s + 4, s + 4));
}

export function PayloadEditor({ payload, pythonAllowed, onChange }: Props) {
  const mode = modeOf(payload);
  // remember what was typed in the other modes while switching between them
  const [kept, setKept] = useState<PayloadDoc>({});
  const [big, setBig] = useState(false); // larger code editor

  const switchTo = (m: Mode) => {
    const saved = { ...kept, ...payload };
    setKept(saved);
    if (m === 'stack') {
      onChange(undefined);
    } else if (m === 'hex') {
      onChange({ hex: saved.hex ?? '' });
    } else {
      onChange({ python: saved.python ?? PYTHON_TEMPLATE });
    }
  };

  const hex = mode === 'hex' ? parseHexInput(payload?.hex ?? '') : null;

  return (
    <div>
      <div className="segmented" role="radiogroup" aria-label="Payload">
        {(
          [
            ['stack', 'Stack default'],
            ['hex', 'Fixed bytes'],
            ['python', 'Python'],
          ] as const
        ).map(([m, label]) => (
          <label key={m} className={mode === m ? 'on' : undefined}>
            <input type="radio" name="payload-mode" checked={mode === m} onChange={() => switchTo(m)} />
            {label}
          </label>
        ))}
      </div>

      {mode === 'stack' && (
        <p className="hint">
          The stack makes up its own test data: <code>len</code> bytes (see the stack’s traffic
          options).
        </p>
      )}

      {mode === 'hex' && (
        <>
          <textarea
            className={`code ${hex?.bytes ? '' : 'invalid'}`}
            rows={3}
            spellCheck={false}
            placeholder="01 A2 FF 00"
            value={payload?.hex ?? ''}
            onChange={(e) => onChange({ hex: e.target.value })}
          />
          <p className="hint" style={hex?.bytes ? undefined : { color: 'var(--danger)' }}>
            {hex?.bytes ? `${hex.bytes.length} bytes, the same in every message` : hex?.error}
          </p>
        </>
      )}

      {mode === 'python' && (
        <>
          <div style={{ position: 'relative' }}>
            <textarea
              className="code"
              rows={9}
              spellCheck={false}
              value={payload?.python ?? ''}
              onChange={(e) => onChange({ python: e.target.value })}
              onKeyDown={(e) => onCodeKey(e, (v) => onChange({ python: v }))}
            />
            <button
              className="expand"
              title="Edit in a larger window"
              onClick={() => setBig(true)}
            >
              ⤢
            </button>
          </div>
          {big && (
            <div className="modal" role="dialog" aria-label="Python payload" onClick={() => setBig(false)}>
              <div className="modal-box" onClick={(e) => e.stopPropagation()}>
                <div className="inline" style={{ justifyContent: 'space-between' }}>
                  <strong style={{ flex: 'none' }}>payload(node, seq, t)</strong>
                  <button className="primary" style={{ flex: 'none' }} onClick={() => setBig(false)}>
                    Done
                  </button>
                </div>
                <textarea
                  className="code"
                  autoFocus
                  spellCheck={false}
                  value={payload?.python ?? ''}
                  onChange={(e) => onChange({ python: e.target.value })}
                  onKeyDown={(e) => {
                    if (e.key === 'Escape') {
                      setBig(false);
                    }
                    onCodeKey(e, (v) => onChange({ python: v }));
                  }}
                />
              </div>
            </div>
          )}
          <p className="hint">
            Called for every message; return <code>bytes</code>. Errors stop the run and show the
            line.
          </p>
          {!pythonAllowed && (
            <p className="hint" style={{ color: 'var(--warn)' }}>
              Running Python needs <code>ksim serve --allow-processes</code> (the code runs on this
              computer).
            </p>
          )}
        </>
      )}
    </div>
  );
}
