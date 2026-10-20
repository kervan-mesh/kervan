/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// The map: nodes on a meter grid, who-hears-whom links, and the replay animation. World
// coordinates are meters with y pointing up (north); the canvas is redrawn on every change.

import { useCallback, useEffect, useRef } from 'react';

import type { Link } from '../api';
import { between, txOverlapping, type Replay, type Rx } from '../model/replay';
import { bounds, type ScenarioDoc } from '../model/scenario';

export type Tool = 'select' | 'add';

interface Props {
  doc: ScenarioDoc;
  links: Link[];
  showLinks: boolean;
  showLosses: boolean;
  showDistances: boolean; // distance labels on links
  replay: Replay | null;
  time: number; // µs
  window: number; // µs an event stays visible
  selected: number | null;
  tool: Tool;
  fitKey: number; // changing it fits the view to the nodes
  nodeNotes: Map<number, string>; // second label line, e.g. duty cycle after a run
  onSelect: (id: number | null) => void;
  onMove: (id: number, x: number, y: number, done: boolean) => void;
  onAdd: (x: number, y: number) => void;
  children?: React.ReactNode; // overlays (tools, legend, banners)
}

interface View {
  ox: number; // screen position of world (0, 0), CSS pixels
  oy: number;
  scale: number; // CSS pixels per meter
}

type Gesture =
  | { kind: 'drag'; id: number; moved: boolean }
  | { kind: 'pan'; x: number; y: number; ox: number; oy: number; moved: boolean };

const NODE_R = 8;

// 1, 2, 5 x 10^k at or above v
export function niceStep(v: number): number {
  const p = Math.pow(10, Math.floor(Math.log10(Math.max(v, 1e-9))));
  for (const m of [1, 2, 5, 10]) {
    if (m * p >= v) {
      return m * p;
    }
  }
  return 10 * p;
}

export function formatDistance(m: number): string {
  return m >= 1000 ? `${+(m / 1000).toFixed(2)} km` : `${+m.toFixed(1)} m`;
}

const APP_LABELS: Record<string, string> = {
  app_tx: 'send',
  app_rx: 'recv ✓',
  app_ack: 'ack ✓',
  app_ack_timeout: 'timeout ✗',
  app_tx_busy: 'queue full',
  kv_relay: 'relay',
  kv_relay_cancel: 'relay skipped',
  kv_relay_drop: 'relay dropped',
  kv_budget_wait: 'duty wait',
  kv_retx: 'retry',
};
const HIDDEN_APP = new Set(['kv_stats', 'kv_log', 'kv_relay_cancel']); // too frequent for the map

function colors(el: Element) {
  const s = getComputedStyle(el);
  const v = (name: string) => s.getPropertyValue(name).trim();
  return {
    bg: v('--field-bg'),
    grid: v('--grid'),
    gridLabel: v('--grid-label'),
    node: v('--node'),
    nodeText: v('--node-text'),
    accent: v('--accent'),
    tx: v('--tx'),
    relay: v('--relay'),
    ok: v('--rx-ok'),
    collision: v('--lost-collision'),
    half: v('--lost-half'),
    off: v('--lost-off'),
    linkGood: v('--link-good'),
    linkFair: v('--link-fair'),
    linkWeak: v('--link-weak'),
  };
}

type Colors = ReturnType<typeof colors>;

function lossColor(c: Colors, rx: Rx): string {
  if (rx.ok) {
    return c.ok;
  }
  return rx.reason === 'collision' ? c.collision : rx.reason === 'half_duplex' ? c.half : c.off;
}

export function Field(props: Props) {
  const wrapRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const view = useRef<View>({ ox: 0, oy: 0, scale: 0.2 });
  const size = useRef({ w: 0, h: 0 });
  const gesture = useRef<Gesture | null>(null);
  const propsRef = useRef(props);
  propsRef.current = props;

  const toScreen = (x: number, y: number) => {
    const v = view.current;
    return { x: v.ox + x * v.scale, y: v.oy - y * v.scale };
  };
  const toWorld = (sx: number, sy: number) => {
    const v = view.current;
    return { x: (sx - v.ox) / v.scale, y: (v.oy - sy) / v.scale };
  };

  const fit = useCallback(() => {
    const { w, h } = size.current;
    if (w === 0 || h === 0) {
      return;
    }
    const b = bounds(propsRef.current.doc);
    const dx = Math.max(b.maxX - b.minX, 20);
    const dy = Math.max(b.maxY - b.minY, 20);
    const pad = 70;
    const scale = Math.min((w - 2 * pad) / dx, (h - 2 * pad) / dy);
    view.current = {
      scale,
      ox: w / 2 - ((b.minX + b.maxX) / 2) * scale,
      oy: h / 2 + ((b.minY + b.maxY) / 2) * scale,
    };
  }, []);

  const draw = useCallback(() => {
    const canvas = canvasRef.current;
    if (!canvas) {
      return;
    }
    const ctx = canvas.getContext('2d');
    if (!ctx) {
      return;
    }
    const p = propsRef.current;
    const { w, h } = size.current;
    const dpr = window.devicePixelRatio || 1;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const c = colors(canvas);
    const v = view.current;

    ctx.fillStyle = c.bg;
    ctx.fillRect(0, 0, w, h);

    // grid
    const step = niceStep(110 / v.scale);
    const tl = toWorld(0, 0);
    const br = toWorld(w, h);
    ctx.lineWidth = 1;
    ctx.strokeStyle = c.grid;
    ctx.fillStyle = c.gridLabel;
    ctx.font = '10px system-ui, sans-serif';
    ctx.beginPath();
    for (let x = Math.ceil(tl.x / step) * step; x <= br.x; x += step) {
      const sx = Math.round(toScreen(x, 0).x) + 0.5;
      ctx.moveTo(sx, 0);
      ctx.lineTo(sx, h);
    }
    for (let y = Math.ceil(br.y / step) * step; y <= tl.y; y += step) {
      const sy = Math.round(toScreen(0, y).y) + 0.5;
      ctx.moveTo(0, sy);
      ctx.lineTo(w, sy);
    }
    ctx.stroke();

    // scale bar
    const barPx = step * v.scale;
    ctx.strokeStyle = c.gridLabel;
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(12, h - 14);
    ctx.lineTo(12 + barPx, h - 14);
    ctx.moveTo(12, h - 18);
    ctx.lineTo(12, h - 10);
    ctx.moveTo(12 + barPx, h - 18);
    ctx.lineTo(12 + barPx, h - 10);
    ctx.stroke();
    ctx.fillText(formatDistance(step), 16, h - 20);

    const pos = new Map<number, { x: number; y: number }>();
    for (const n of p.doc.nodes) {
      pos.set(n.id, toScreen(n.pos[0] ?? 0, n.pos[1] ?? 0));
    }

    // links: one line per pair, solid if both directions work, dashed if only one
    if (p.showLinks) {
      const pairs = new Map<string, { a: number; b: number; snr: number[] }>();
      for (const l of p.links) {
        const a = Math.min(l.from, l.to);
        const b = Math.max(l.from, l.to);
        const key = `${a}-${b}`;
        const e = pairs.get(key) ?? { a, b, snr: [] };
        e.snr.push(l.snr);
        pairs.set(key, e);
      }
      ctx.lineWidth = 1.5;
      ctx.globalAlpha = 0.45;
      for (const e of pairs.values()) {
        const pa = pos.get(e.a);
        const pb = pos.get(e.b);
        if (!pa || !pb) {
          continue;
        }
        const worst = Math.min(...e.snr);
        ctx.strokeStyle = worst >= 5 ? c.linkGood : worst >= -5 ? c.linkFair : c.linkWeak;
        ctx.setLineDash(e.snr.length === 2 ? [] : [5, 4]);
        ctx.beginPath();
        ctx.moveTo(pa.x, pa.y);
        ctx.lineTo(pb.x, pb.y);
        ctx.stroke();
      }
      ctx.setLineDash([]);
      ctx.globalAlpha = 1;

      // distance labels in the middle of each link, on top of all lines
      if (p.showDistances) {
        const world = new Map(p.doc.nodes.map((n) => [n.id, n.pos]));
        ctx.font = '10px system-ui, sans-serif';
        ctx.textAlign = 'center';
        ctx.textBaseline = 'middle';
        for (const e of pairs.values()) {
          const pa = pos.get(e.a);
          const pb = pos.get(e.b);
          const wa = world.get(e.a);
          const wb = world.get(e.b);
          if (!pa || !pb || !wa || !wb || Math.hypot(pb.x - pa.x, pb.y - pa.y) < 60) {
            continue; // no room for a label on short lines
          }
          const d = Math.hypot(
            (wb[0] ?? 0) - (wa[0] ?? 0),
            (wb[1] ?? 0) - (wa[1] ?? 0),
            (wb[2] ?? 0) - (wa[2] ?? 0),
          );
          const text = formatDistance(d);
          const mx = (pa.x + pb.x) / 2;
          const my = (pa.y + pb.y) / 2;
          const w = ctx.measureText(text).width + 6;
          ctx.globalAlpha = 0.85;
          ctx.fillStyle = c.bg;
          ctx.fillRect(mx - w / 2, my - 7, w, 14);
          ctx.globalAlpha = 1;
          ctx.fillStyle = c.gridLabel;
          ctx.fillText(text, mx, my);
        }
        ctx.textAlign = 'start';
        ctx.textBaseline = 'alphabetic';
      }
    }

    // replay
    const onAir = new Map<number, string>(); // node -> color
    if (p.replay) {
      const T = p.time;
      const W = p.window;
      for (const tx of txOverlapping(p.replay, T - W, T)) {
        const span = Math.max(tx.end - tx.t, W);
        if (T < tx.t || T > tx.t + span) {
          continue;
        }
        const q = (T - tx.t) / span;
        const at = pos.get(tx.node);
        if (!at) {
          continue;
        }
        const col = tx.relay ? c.relay : c.tx;
        if (T <= tx.end || q < 0.5) {
          onAir.set(tx.node, col);
        }
        ctx.strokeStyle = col;
        ctx.globalAlpha = 0.8 * (1 - q);
        ctx.lineWidth = 2;
        ctx.beginPath();
        ctx.arc(at.x, at.y, NODE_R + 4 + q * 46, 0, Math.PI * 2);
        ctx.stroke();
      }
      ctx.globalAlpha = 1;

      for (const rx of between(p.replay.rxs, T - W, T)) {
        if (!rx.ok && !p.showLosses) {
          continue;
        }
        const a = pos.get(rx.from);
        const b = pos.get(rx.node);
        if (!a || !b) {
          continue;
        }
        const fade = 1 - (T - rx.t) / W;
        const dx = b.x - a.x;
        const dy = b.y - a.y;
        const len = Math.hypot(dx, dy);
        if (len < 2 * NODE_R) {
          continue;
        }
        const ux = dx / len;
        const uy = dy / len;
        const sx = a.x + ux * (NODE_R + 2);
        const sy = a.y + uy * (NODE_R + 2);
        const ex = b.x - ux * (NODE_R + 3);
        const ey = b.y - uy * (NODE_R + 3);
        const col = lossColor(c, rx);
        ctx.globalAlpha = Math.max(0, fade);
        ctx.strokeStyle = col;
        ctx.fillStyle = col;
        ctx.lineWidth = rx.ok ? 2.5 : 1.5;
        ctx.setLineDash(rx.ok ? [] : [4, 3]);
        ctx.beginPath();
        ctx.moveTo(sx, sy);
        ctx.lineTo(ex, ey);
        ctx.stroke();
        ctx.setLineDash([]);
        if (rx.ok) {
          ctx.beginPath(); // arrow head
          ctx.moveTo(ex, ey);
          ctx.lineTo(ex - ux * 9 - uy * 4.5, ey - uy * 9 + ux * 4.5);
          ctx.lineTo(ex - ux * 9 + uy * 4.5, ey - uy * 9 - ux * 4.5);
          ctx.closePath();
          ctx.fill();
        } else {
          const k = 4; // cross at the receiver
          ctx.lineWidth = 2;
          ctx.beginPath();
          ctx.moveTo(ex - k, ey - k);
          ctx.lineTo(ex + k, ey + k);
          ctx.moveTo(ex + k, ey - k);
          ctx.lineTo(ex - k, ey + k);
          ctx.stroke();
        }
      }
      ctx.globalAlpha = 1;
    }

    // nodes
    ctx.textAlign = 'center';
    for (const n of p.doc.nodes) {
      const at = pos.get(n.id)!;
      const sel = n.id === p.selected;
      if (sel) {
        ctx.strokeStyle = c.accent;
        ctx.lineWidth = 3;
        ctx.beginPath();
        ctx.arc(at.x, at.y, NODE_R + 4, 0, Math.PI * 2);
        ctx.stroke();
      }
      ctx.fillStyle = onAir.get(n.id) ?? c.node;
      ctx.beginPath();
      ctx.arc(at.x, at.y, NODE_R, 0, Math.PI * 2);
      ctx.fill();
      ctx.fillStyle = c.nodeText;
      ctx.font = `${sel ? 600 : 500} 11px system-ui, sans-serif`;
      ctx.fillText(String(n.id), at.x, at.y + NODE_R + 13);
      const note = p.nodeNotes.get(n.id);
      if (note) {
        ctx.fillStyle = c.gridLabel;
        ctx.font = '10px system-ui, sans-serif';
        ctx.fillText(note, at.x, at.y + NODE_R + 25);
      }
    }

    // app events as fading labels above the node, newest at the bottom
    if (p.replay) {
      const T = p.time;
      const W = 2 * p.window;
      const perNode = new Map<number, { text: string; fade: number }[]>();
      for (const a of between(p.replay.apps, T - W, T)) {
        if (HIDDEN_APP.has(a.name)) {
          continue;
        }
        const list = perNode.get(a.node) ?? [];
        list.push({ text: APP_LABELS[a.name] ?? a.name, fade: 1 - (T - a.t) / W });
        perNode.set(a.node, list);
      }
      ctx.font = '600 11px system-ui, sans-serif';
      for (const [id, list] of perNode) {
        const at = pos.get(id);
        if (!at) {
          continue;
        }
        list.slice(-3).forEach((item, i, shown) => {
          ctx.globalAlpha = Math.max(0, Math.min(1, item.fade * 1.5));
          ctx.fillStyle = item.text.includes('✗') || item.text.includes('drop')
            ? c.collision
            : item.text === 'relay'
              ? c.relay
              : c.nodeText;
          ctx.fillText(item.text, at.x, at.y - NODE_R - 6 - (shown.length - 1 - i) * 13);
        });
      }
      ctx.globalAlpha = 1;
    }
    ctx.textAlign = 'start';
  }, []);

  // size the canvas to its box
  useEffect(() => {
    const wrap = wrapRef.current;
    const canvas = canvasRef.current;
    if (!wrap || !canvas) {
      return;
    }
    let first = true;
    const ro = new ResizeObserver(() => {
      const r = wrap.getBoundingClientRect();
      const dpr = window.devicePixelRatio || 1;
      size.current = { w: r.width, h: r.height };
      canvas.width = Math.round(r.width * dpr);
      canvas.height = Math.round(r.height * dpr);
      if (first) {
        fit();
        first = false;
      }
      draw();
    });
    ro.observe(wrap);
    const media = window.matchMedia('(prefers-color-scheme: dark)');
    media.addEventListener('change', draw);
    return () => {
      ro.disconnect();
      media.removeEventListener('change', draw);
    };
  }, [draw, fit]);

  useEffect(() => {
    fit();
    draw();
  }, [props.fitKey, fit, draw]);

  useEffect(draw);

  const hit = (sx: number, sy: number): number | null => {
    let best: number | null = null;
    let bestD = 14;
    for (const n of propsRef.current.doc.nodes) {
      const s = toScreen(n.pos[0] ?? 0, n.pos[1] ?? 0);
      const d = Math.hypot(s.x - sx, s.y - sy);
      if (d < bestD) {
        bestD = d;
        best = n.id;
      }
    }
    return best;
  };

  const local = (e: React.PointerEvent | React.WheelEvent) => {
    const r = canvasRef.current!.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  };

  const snap = (v: number) => {
    const grid = niceStep(4 / view.current.scale);
    return Math.round(v / grid) * grid;
  };

  const onPointerDown = (e: React.PointerEvent) => {
    const s = local(e);
    canvasRef.current!.setPointerCapture(e.pointerId);
    const id = hit(s.x, s.y);
    if (id !== null) {
      props.onSelect(id);
      gesture.current = { kind: 'drag', id, moved: false };
    } else if (props.tool === 'add') {
      const wpt = toWorld(s.x, s.y);
      props.onAdd(snap(wpt.x), snap(wpt.y));
    } else {
      const v = view.current;
      gesture.current = { kind: 'pan', x: s.x, y: s.y, ox: v.ox, oy: v.oy, moved: false };
    }
  };

  const onPointerMove = (e: React.PointerEvent) => {
    const g = gesture.current;
    if (!g) {
      return;
    }
    const s = local(e);
    if (g.kind === 'drag') {
      const wpt = toWorld(s.x, s.y);
      g.moved = true;
      props.onMove(g.id, snap(wpt.x), snap(wpt.y), false);
    } else {
      if (Math.hypot(s.x - g.x, s.y - g.y) > 3) {
        g.moved = true;
      }
      view.current = { ...view.current, ox: g.ox + s.x - g.x, oy: g.oy + s.y - g.y };
      draw();
    }
  };

  const onPointerUp = (e: React.PointerEvent) => {
    const g = gesture.current;
    gesture.current = null;
    if (!g) {
      return;
    }
    if (g.kind === 'drag' && g.moved) {
      const s = local(e);
      const wpt = toWorld(s.x, s.y);
      props.onMove(g.id, snap(wpt.x), snap(wpt.y), true);
    } else if (g.kind === 'pan' && !g.moved) {
      props.onSelect(null);
    }
  };

  const onWheel = (e: React.WheelEvent) => {
    const s = local(e);
    const before = toWorld(s.x, s.y);
    const factor = Math.pow(1.0015, -e.deltaY);
    const scale = Math.min(Math.max(view.current.scale * factor, 1e-4), 200);
    view.current = { scale, ox: s.x - before.x * scale, oy: s.y + before.y * scale };
    draw();
  };

  return (
    <div className="field-wrap" ref={wrapRef}>
      <canvas
        ref={canvasRef}
        style={{ cursor: props.tool === 'add' ? 'crosshair' : 'default' }}
        onPointerDown={onPointerDown}
        onPointerMove={onPointerMove}
        onPointerUp={onPointerUp}
        onWheel={onWheel}
        aria-label="Scenario map"
      />
      {props.children}
    </div>
  );
}
