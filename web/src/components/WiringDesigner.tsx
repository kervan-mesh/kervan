/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// The wiring of a radio module to an MCU board, drawn from the hardware catalog (pin headers as
// seen from above), with the pins to choose and the firmware to build for them.

import { useEffect, useMemo, useState } from 'react';

import { wiringFor, type McuInfo, type ModuleInfo, type Wiring, type WiringRow } from '../api';
import { withField, type NodeDoc } from '../model/scenario';
import {
  choosePin,
  isGpio,
  layoutHeader,
  movable,
  optionsFor,
  pinTitle,
  wireColor,
  type PlacedPin,
} from '../model/wiring';
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
  const layout = layoutHeader(mcu);
  const bottomTop = 34 + Math.max(layout.rows - 1, 0) * PITCH + (layout.bottomRows > 0 ? 34 : 0);
  const bw = Math.max(BOARD.w, 40 + layout.bottomCols * PITCH);
  // a second bottom row writes its labels downwards, below its pins
  const bh = bottomTop + Math.max(layout.bottomRows - 1, 0) * PITCH + 34 + (layout.bottomRows > 1 ? 34 : 0);
  const pinPos = (p: PlacedPin): Point => {
    if (p.side === 'bottom') {
      return { x: BOARD.x + 20 + p.col * PITCH, y: BOARD.y + bottomTop + p.row * PITCH };
    }
    return {
      x: p.side === 'left' ? BOARD.x + 12 : BOARD.x + bw - 12,
      y: BOARD.y + 34 + p.row * PITCH,
    };
  };

  // module pads, row by row; long single columns (pin lists) get tighter rows
  const cols = Math.max(...module.header.map((r) => r.length));
  const tight = module.header.length > 10;
  const cellH = tight ? 14 : CELL.h;
  const gapY = tight ? 3 : CELL.gapY;
  const longest = Math.max(...module.header.flat().map((p) => p.label.length));
  const cellW = Math.max(CELL.w, longest * 6 + 10);
  const mw = 24 + cols * (cellW + CELL.gapX) + 70;
  const mh = 46 + module.header.length * (cellH + gapY);
  const mx = BOARD.x + bw + 150;
  const my = Math.max(BOARD.y, BOARD.y + bh / 2 - mh / 2);
  const cell = (r: number, c: number): Point => ({
    x: mx + 14 + c * (cellW + CELL.gapX),
    y: my + 32 + r * (cellH + gapY),
  });
  const cellOf = new Map<string, Point>(); // the first pad of each name gets the wire
  module.header.forEach((row, r) =>
    row.forEach((p, c) => {
      if (!cellOf.has(p.name)) {
        cellOf.set(p.name, cell(r, c));
      }
    }),
  );
  const wired = new Set(rows.map((r) => r.module_pin));

  // a wire per row, from the MCU pin to the module pad
  const at = new Map(layout.pins.map((p) => [`${p.strip}:${p.index}`, p]));
  const usedBy = new Map<string, WiringRow>();
  let lane = 0;
  const wires = rows.flatMap((r) => {
    const p =
      at.get(`${r.mcu_strip}:${r.mcu_index}`) ?? layout.pins.find((x) => x.name === r.mcu_name);
    const box = cellOf.get(r.module_pin);
    if (!p || !box) {
      return [];
    }
    usedBy.set(`${p.strip}:${p.index}`, r);
    const s = pinPos(p);
    const t = { x: box.x + cellW / 2, y: box.y + cellH / 2 };
    let d: string;
    if (p.side === 'right') {
      d = `M${s.x} ${s.y} C${s.x + 80} ${s.y}, ${t.x - 90} ${t.y}, ${t.x} ${t.y}`;
    } else {
      // around the bottom of the board
      const k = lane++;
      const ly = BOARD.y + bh + 14 + k * 7;
      const start =
        p.side === 'left' ? `M${s.x} ${s.y} L${BOARD.x - 18 - k * 7} ${s.y} L${BOARD.x - 18 - k * 7} ${ly}` : `M${s.x} ${s.y} L${s.x} ${ly}`;
      d = `${start} L${mx - 40 - k * 7} ${ly} C${mx - 10} ${ly}, ${t.x - 60} ${t.y}, ${t.x} ${t.y}`;
    }
    return [{ row: r, d }];
  });
  const height = Math.max(BOARD.y + bh + 26 + lane * 7, my + mh + 24);
  const width = mx + mw + 10;
  const dim = (pin: string) => (hover !== null && hover !== pin ? 0.25 : 1);
  const edge = (text: string, y: number) =>
    text && (
      <>
        <rect x={BOARD.x + bw / 2 - 22} y={y} width={44} height={14} rx={3} fill="#9aa4ae" />
        <text x={BOARD.x + bw / 2} y={y + 10} className="wd-edge" textAnchor="middle">
          {text}
        </text>
      </>
    );

  return (
    <svg
      className="wiring-drawing"
      viewBox={`0 0 ${width} ${height}`}
      role="img"
      aria-label={`${module.title} wired to ${mcu.title}`}
      onMouseLeave={() => onHover(null)}
    >
      {/* MCU board */}
      <rect x={BOARD.x} y={BOARD.y} width={bw} height={bh} rx={8} className="wd-mcu" />
      {edge(mcu.header!.top, BOARD.y - 8)}
      {edge(mcu.header!.bottom, BOARD.y + bh - 6)}
      <text
        className="wd-title"
        textAnchor="middle"
        transform={`translate(${BOARD.x + bw / 2} ${BOARD.y + bottomTop / 2 + 10}) rotate(-90)`}
      >
        {mcu.title}
      </text>
      {layout.strips.map(
        (s, k) =>
          s.name &&
          s.side !== 'bottom' && (
            <text
              key={k}
              x={s.side === 'left' ? BOARD.x + 26 : BOARD.x + bw - 26}
              y={BOARD.y + 34 + s.row * PITCH - 10}
              className="wd-strip"
              textAnchor={s.side === 'left' ? 'start' : 'end'}
            >
              {s.name}
            </text>
          ),
      )}
      {layout.pins.map((p) => {
        const pos = pinPos(p);
        const r = usedBy.get(`${p.strip}:${p.index}`);
        const pickable = selected !== null && isGpio(mcu, p.name);
        const labelAt: { x: number; y: number; anchor: 'start' | 'end' } =
          p.side === 'left'
            ? { x: pos.x + 10, y: pos.y + 3, anchor: 'start' }
            : p.side === 'right'
              ? { x: pos.x - 10, y: pos.y + 3, anchor: 'end' }
              : p.row === 0
                ? { x: pos.x + 3, y: pos.y - 9, anchor: 'start' }
                : { x: pos.x + 3, y: pos.y + 9, anchor: 'end' };
        return (
          <g
            key={`${p.strip}:${p.index}`}
            className={pickable ? 'wd-pick' : undefined}
            onClick={() => pickable && onPickMcu(p.name)}
            onMouseEnter={() => onHover(r ? r.module_pin : null)}
          >
            <title>{`${pinTitle(mcu, p.name)}${r ? ` — ${r.module_pin}` : ''}`}</title>
            <circle
              cx={pos.x}
              cy={pos.y}
              r={r ? 5 : 4}
              fill={r ? wireColor(r.role) : 'var(--panel)'}
              stroke={r ? wireColor(r.role) : p.name.includes('GND') ? '#768390' : 'var(--muted)'}
              strokeWidth={1.5}
              opacity={r ? dim(r.module_pin) : 1}
            />
            <text
              x={labelAt.x}
              y={labelAt.y}
              className={r ? 'wd-pin wd-used' : 'wd-pin'}
              textAnchor={labelAt.anchor}
              transform={p.side === 'bottom' ? `rotate(-90 ${labelAt.x} ${labelAt.y})` : undefined}
            >
              {p.label}
            </text>
            {p.number > 0 && p.side !== 'bottom' && (
              <text
                x={p.side === 'left' ? BOARD.x - 4 : BOARD.x + bw + 4}
                y={pos.y + 3}
                className="wd-small"
                textAnchor={p.side === 'left' ? 'end' : 'start'}
              >
                {p.number}
              </text>
            )}
          </g>
        );
      })}

      {/* wires under the module's pads */}
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
        row.map((p, c) => {
          const pos = cell(r, c);
          const wrow = rows.find((x) => x.module_pin === p.name);
          const live = wrow !== undefined && cellOf.get(p.name) === pos;
          const color = live ? wireColor(wrow.role) : '#768390';
          const sel = selected === p.name;
          return (
            <g
              key={`${r}:${c}`}
              className={live && movable(wrow.role) ? 'wd-pick' : undefined}
              onClick={() => live && movable(wrow.role) && onPickModule(p.name)}
              onMouseEnter={() => onHover(wired.has(p.name) ? p.name : null)}
            >
              <title>
                {wrow ? `${p.label} → ${wrow.mcu_pin || 'not connected'}` : `${p.label}: not used`}
                {live && movable(wrow.role) ? ' — click, then click a pin of the board' : ''}
              </title>
              <rect
                x={pos.x}
                y={pos.y}
                width={cellW}
                height={cellH}
                rx={3}
                fill={sel ? color : '#0d1117'}
                stroke={color}
                strokeWidth={sel || (live && hover === p.name) ? 2.5 : 1.5}
                opacity={live || !wired.has(p.name) ? 1 : 0.6}
              />
              <text x={pos.x + cellW / 2} y={pos.y + cellH / 2 + 3.5} className="wd-cell" textAnchor="middle">
                {p.label}
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
      .catch(async (e: unknown) => {
        if (!live) {
          return;
        }
        setError(e instanceof Error ? e.message : String(e));
        // draw the default wiring meanwhile, so the drawing never disappears
        try {
          const w = await wiringFor(mcu.name, module.name, {});
          if (live) {
            setWiring(w);
          }
        } catch {
          setWiring(null);
        }
      });
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
      {error && (
        <p className="warning">
          ⚠ {error}. The drawing shows the default wiring: press Default pins to use it.
        </p>
      )}
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
                        {pinTitle(mcu, o)}
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
          port={node.hardware?.port}
        />
      )}
    </div>
  );
}
