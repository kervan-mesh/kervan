/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// The wiring of a radio module to an MCU board, drawn from the hardware catalog (pin headers as
// seen from above), with the pins to choose and the firmware to build for them.

import { useEffect, useMemo, useState } from 'react';

import { wiringFor, type McuInfo, type ModuleInfo, type Wiring, type WiringRow } from '../api';
import { withField, type NodeDoc } from '../model/scenario';
import { choosePin, headerPins, isGpio, movable, optionsFor, wireColor } from '../model/wiring';
import { FirmwareBuilder } from './FirmwareBuilder';

interface Props {
  node: NodeDoc;
  mcu: McuInfo;
  module: ModuleInfo;
  buildAllowed: boolean;
  onChange: (n: NodeDoc) => void;
  big?: boolean;
}

const PITCH = 17;
const BOARD = { x: 76, y: 36, w: 150 };
const CELL = { w: 42, h: 16, gapX: 6, gapY: 8 };

interface Point {
  x: number;
  y: number;
}

function Drawing({
  mcu,
  module,
  rows,
  hover,
  selected,
  onHover,
  onPickModule,
  onPickMcu,
}: {
  mcu: McuInfo;
  module: ModuleInfo;
  rows: WiringRow[];
  hover: string | null;
  selected: string | null;
  onHover: (pin: string | null) => void;
  onPickModule: (pin: string) => void;
  onPickMcu: (name: string) => void;
}) {
  const pins = headerPins(mcu);
  const n = mcu.header!.left.length;
  const bh = 34 + (n - 1) * PITCH + 34;
  const pinPos = (side: 'left' | 'right', index: number): Point => ({
    x: side === 'left' ? BOARD.x + 12 : BOARD.x + BOARD.w - 12,
    y: BOARD.y + 34 + index * PITCH,
  });

  const cols = Math.max(...module.header.map((r) => r.length));
  const mw = 24 + cols * (CELL.w + CELL.gapX) + 70;
  const mh = 46 + module.header.length * (CELL.h + CELL.gapY);
  const mx = BOARD.x + BOARD.w + 150;
  const my = BOARD.y + bh / 2 - mh / 2;
  const cell = (r: number, c: number): Point => ({
    x: mx + 14 + c * (CELL.w + CELL.gapX),
    y: my + 32 + r * (CELL.h + CELL.gapY),
  });
  const cellOf = new Map<string, Point>();
  module.header.forEach((row, r) => row.forEach((name, c) => cellOf.set(name, cell(r, c))));

  // a wire per row, ending at the module pin's box
  const byName = new Map(pins.map((p) => [p.name, p]));
  const usedBy = new Map<string, WiringRow>();
  let lane = 0;
  const wires = rows.flatMap((r) => {
    // supply pins name a header pin that appears more than once (GND): use the listed number
    const p =
      pins.find((x) => x.name === r.mcu_name && (r.mcu_number === 0 || x.number === r.mcu_number)) ??
      byName.get(r.mcu_name);
    const box = cellOf.get(r.module_pin);
    if (!p || !box) {
      return [];
    }
    usedBy.set(`${p.side}${p.index}`, r);
    const s = pinPos(p.side, p.index);
    const t = { x: box.x + CELL.w / 2, y: box.y + CELL.h / 2 };
    let d: string;
    if (p.side === 'right') {
      d = `M${s.x} ${s.y} C${s.x + 80} ${s.y}, ${t.x - 90} ${t.y}, ${t.x} ${t.y}`;
    } else {
      // around the bottom of the board
      const k = lane++;
      const lx = BOARD.x - 18 - k * 7;
      const ly = BOARD.y + bh + 14 + k * 7;
      d = `M${s.x} ${s.y} L${lx} ${s.y} L${lx} ${ly} L${mx - 40 - k * 7} ${ly} C${mx - 10} ${ly}, ${t.x - 60} ${t.y}, ${t.x} ${t.y}`;
    }
    return [{ row: r, d }];
  });
  const height = BOARD.y + bh + 26 + lane * 7;
  const width = mx + mw + 10;
  const dim = (pin: string) => (hover !== null && hover !== pin ? 0.25 : 1);

  return (
    <svg
      className="wiring-drawing"
      viewBox={`0 0 ${width} ${height}`}
      role="img"
      aria-label={`${module.title} wired to ${mcu.title}`}
      onMouseLeave={() => onHover(null)}
    >
      {/* MCU board */}
      <rect x={BOARD.x + BOARD.w / 2 - 18} y={BOARD.y - 12} width={36} height={18} rx={3} fill="#9aa4ae" />
      <text x={BOARD.x + BOARD.w / 2} y={BOARD.y - 16} className="wd-small" textAnchor="middle">
        {mcu.header!.top}
      </text>
      <rect x={BOARD.x} y={BOARD.y} width={BOARD.w} height={bh} rx={8} className="wd-mcu" />
      <text
        className="wd-title"
        textAnchor="middle"
        transform={`translate(${BOARD.x + BOARD.w / 2} ${BOARD.y + bh / 2}) rotate(-90)`}
      >
        {mcu.title}
      </text>
      {pins.map((p) => {
        const at = pinPos(p.side, p.index);
        const r = usedBy.get(`${p.side}${p.index}`);
        const gpio = isGpio(p.name);
        const pickable = selected !== null && gpio;
        return (
          <g
            key={`${p.side}${p.index}`}
            className={pickable ? 'wd-pick' : undefined}
            onClick={() => pickable && onPickMcu(p.name)}
            onMouseEnter={() => onHover(r ? r.module_pin : null)}
          >
            <title>{`${p.name} (pin ${p.number})${r ? ` — ${r.module_pin}` : ''}`}</title>
            <circle
              cx={at.x}
              cy={at.y}
              r={r ? 5 : 4}
              fill={r ? wireColor(r.role) : 'var(--panel)'}
              stroke={r ? wireColor(r.role) : p.name.includes('GND') ? '#768390' : 'var(--muted)'}
              strokeWidth={1.5}
              opacity={r ? dim(r.module_pin) : 1}
            />
            <text
              x={p.side === 'left' ? at.x + 10 : at.x - 10}
              y={at.y + 3}
              className={r ? 'wd-pin wd-used' : 'wd-pin'}
              textAnchor={p.side === 'left' ? 'start' : 'end'}
            >
              {p.name}
            </text>
            <text
              x={p.side === 'left' ? BOARD.x - 4 : BOARD.x + BOARD.w + 4}
              y={at.y + 3}
              className="wd-small"
              textAnchor={p.side === 'left' ? 'end' : 'start'}
            >
              {p.number}
            </text>
          </g>
        );
      })}

      {/* wires under the module's pin boxes */}
      {wires.map((w) => (
        <path
          key={w.row.module_pin}
          d={w.d}
          fill="none"
          stroke={wireColor(w.row.role)}
          strokeWidth={hover === w.row.module_pin ? 4 : 2.5}
          strokeDasharray={w.row.role === 'gpio_in_optional' ? '5 4' : undefined}
          strokeLinejoin="round"
          opacity={dim(w.row.module_pin)}
          onMouseEnter={() => onHover(w.row.module_pin)}
        />
      ))}

      {/* radio module */}
      <rect x={mx} y={my} width={mw} height={mh} rx={6} className="wd-module" />
      <path
        d={`M${mx + mw - 52} ${my + 14} h14 v${mh - 40} h10 v-${mh - 40} h10 v${mh - 40} h10`}
        fill="none"
        stroke="#c9a227"
        strokeWidth={2}
      />
      <text x={mx + 12} y={my + 18} className="wd-title" textAnchor="start">
        {module.chip}
      </text>
      {module.header.map((row, r) =>
        row.map((name, c) => {
          const at = cell(r, c);
          const wrow = rows.find((x) => x.module_pin === name);
          const color = wrow ? wireColor(wrow.role) : '#768390';
          const sel = selected === name;
          return (
            <g
              key={name}
              className="wd-pick"
              onClick={() => wrow && movable(wrow.role) && onPickModule(name)}
              onMouseEnter={() => onHover(name)}
            >
              <title>
                {wrow ? `${name} → ${wrow.mcu_pin || 'not connected'}` : name}
                {wrow && movable(wrow.role) ? ' — click, then click a pin of the board' : ''}
              </title>
              <rect
                x={at.x}
                y={at.y}
                width={CELL.w}
                height={CELL.h}
                rx={3}
                fill={sel ? color : '#0d1117'}
                stroke={color}
                strokeWidth={sel || hover === name ? 2.5 : 1.5}
              />
              <text x={at.x + CELL.w / 2} y={at.y + 11.5} className="wd-cell" textAnchor="middle">
                {name}
              </text>
            </g>
          );
        }),
      )}
      <text x={mx + mw / 2} y={my + mh + 14} className="wd-small" textAnchor="middle">
        {module.title}
      </text>
    </svg>
  );
}

export function WiringDesigner({ node, mcu, module, buildAllowed, onChange, big }: Props) {
  const [wiring, setWiring] = useState<Wiring | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [hover, setHover] = useState<string | null>(null);
  const [selected, setSelected] = useState<string | null>(null);
  const choice = useMemo(() => node.pins ?? {}, [node.pins]);
  const key = JSON.stringify(choice);

  useEffect(() => {
    let live = true;
    wiringFor(mcu.name, module.name, choice)
      .then((w) => {
        if (live) {
          setWiring(w);
          setError(null);
        }
      })
      .catch((e: unknown) => live && setError(e instanceof Error ? e.message : String(e)));
    return () => {
      live = false;
    };
  }, [mcu.name, module.name, key]);

  // Esc cancels a pick (before the app's own Esc, which would deselect the node)
  useEffect(() => {
    if (selected === null) {
      return;
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        e.stopPropagation();
        setSelected(null);
      }
    };
    window.addEventListener('keydown', onKey, true);
    return () => window.removeEventListener('keydown', onKey, true);
  }, [selected]);

  const choose = (pin: string, to: string) => {
    const rows = wiring?.rows ?? [];
    const next = choosePin(mcu, rows, choice, pin, to);
    onChange(withField(node, 'pins', Object.keys(next).length > 0 ? next : undefined));
    setSelected(null);
  };

  const rows = wiring?.rows ?? [];
  return (
    <div className={big ? 'wiring-designer big' : 'wiring-designer'}>
      {mcu.header && module.header.length > 0 && rows.length > 0 && (
        <Drawing
          mcu={mcu}
          module={module}
          rows={rows}
          hover={hover}
          selected={selected}
          onHover={setHover}
          onPickModule={(p) => setSelected(selected === p ? null : p)}
          onPickMcu={(name) => selected && choose(selected, name)}
        />
      )}
      <p className="hint">
        {selected
          ? `Now click the board pin for ${selected} (Esc: cancel).`
          : 'Click a module pin, then a pin of the board, or pick pins in the table.'}
      </p>
      {error && <p className="warning">⚠ {error}</p>}
      {wiring?.warnings.map((w) => (
        <p key={w} className="warning">
          ⚠ {w}
        </p>
      ))}

      <table className="stats wiring">
        <thead>
          <tr>
            <th>module</th>
            <th>{mcu.title}</th>
          </tr>
        </thead>
        <tbody>
          {rows.map((r) => (
            <tr
              key={r.module_pin}
              onMouseEnter={() => setHover(r.module_pin)}
              onMouseLeave={() => setHover(null)}
              className={hover === r.module_pin ? 'selected' : undefined}
            >
              <td>
                <span className="swatch" style={{ background: wireColor(r.role) }} />
                {r.module_pin}
              </td>
              <td>
                {movable(r.role) && mcu.header ? (
                  <select
                    value={r.mcu_name}
                    aria-label={`${r.module_pin} goes to`}
                    onChange={(e) => choose(r.module_pin, e.target.value)}
                  >
                    {optionsFor(mcu, r.role).map((o) => (
                      <option key={o} value={o}>
                        {o} (pin {headerPins(mcu).find((p) => p.name === o)?.number ?? '?'})
                      </option>
                    ))}
                  </select>
                ) : (
                  r.mcu_pin || '–'
                )}
                {r.role === 'gpio_in_optional' && <span className="hint"> optional</span>}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      {node.pins && (
        <button onClick={() => onChange(withField(node, 'pins', undefined))}>Default pins</button>
      )}
      {wiring?.spi && <p className="hint">SCK, MOSI and MISO use {wiring.spi}.</p>}
      {wiring && wiring.notes.length > 0 && (
        <ul className="notes">
          {wiring.notes.map((x) => (
            <li key={x}>{x}</li>
          ))}
        </ul>
      )}

      {wiring?.firmware && (
        <FirmwareBuilder
          mcu={mcu.name}
          module={module.name}
          pins={choice}
          firmware={wiring.firmware}
          allowed={buildAllowed}
        />
      )}
    </div>
  );
}
