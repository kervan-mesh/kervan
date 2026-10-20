/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Replay controls, an activity strip over the whole run (click to jump), and the events just
// before the cursor.

import { useEffect, useMemo, useRef } from 'react';

import {
  bins,
  formatTime,
  LOSS_LABEL,
  lowerBound,
  nextEventTime,
  txOfReception,
  type Replay,
  type Tx,
} from '../model/replay';

export const SPEEDS = [0.1, 1, 10, 60, 300, 1000];

interface Props {
  replay: Replay;
  time: number;
  playing: boolean;
  speed: number;
  selected: number | null;
  onSeek: (t: number) => void;
  onPlay: (playing: boolean) => void;
  onSpeed: (speed: number) => void;
  onSelect: (id: number) => void;
  onPacket: (tx: Tx) => void;
}

function Activity({ replay, time, onSeek }: Pick<Props, 'replay' | 'time' | 'onSeek'>) {
  const ref = useRef<HTMLCanvasElement>(null);
  const data = useMemo(() => bins(replay, 300), [replay]);

  useEffect(() => {
    const canvas = ref.current;
    if (!canvas) {
      return;
    }
    const r = canvas.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    canvas.width = Math.round(r.width * dpr);
    canvas.height = Math.round(r.height * dpr);
    const ctx = canvas.getContext('2d');
    if (!ctx) {
      return;
    }
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    const css = getComputedStyle(canvas);
    const col = (n: string) => css.getPropertyValue(n).trim();
    const w = r.width;
    const h = r.height;
    ctx.clearRect(0, 0, w, h);
    const max = Math.max(1, ...data.map((b) => b.tx));
    const bw = w / data.length;
    data.forEach((b, i) => {
      const x = i * bw;
      const th = (b.tx / max) * (h - 12);
      ctx.fillStyle = col('--tx');
      ctx.globalAlpha = 0.65;
      ctx.fillRect(x, h - th, Math.max(1, bw - 0.5), th);
      const lost = b.collision + b.halfDuplex;
      if (lost > 0) {
        ctx.globalAlpha = 1;
        ctx.fillStyle = b.collision > 0 ? col('--lost-collision') : col('--lost-half');
        ctx.fillRect(x, 0, Math.max(1, bw - 0.5), 4 + Math.min(6, lost));
      }
    });
    ctx.globalAlpha = 1;
    const cx = (time / Math.max(1, replay.duration)) * w;
    ctx.strokeStyle = col('--accent');
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(cx, 0);
    ctx.lineTo(cx, h);
    ctx.stroke();
  }, [data, time, replay.duration]);

  return (
    <canvas
      ref={ref}
      className="activity"
      title="Frames over time (red marks: losses). Click to jump."
      onPointerDown={(e) => {
        const r = e.currentTarget.getBoundingClientRect();
        onSeek(((e.clientX - r.left) / r.width) * replay.duration);
      }}
    />
  );
}

interface LogLine {
  t: number;
  node: number;
  cls: string;
  text: string;
  tx?: Tx; // the frame, for radio events
}

function recentEvents(r: Replay, time: number, node: number | null, limit: number): LogLine[] {
  const out: LogLine[] = [];
  const involves = (...ids: number[]) => node === null || ids.includes(node);
  let i = lowerBound(r.txs, time + 1) - 1;
  let j = lowerBound(r.rxs, time + 1) - 1;
  let k = lowerBound(r.apps, time + 1) - 1;
  // merge the three sorted arrays backwards from the cursor
  while (out.length < limit && (i >= 0 || j >= 0 || k >= 0)) {
    const ti = i >= 0 ? r.txs[i]!.t : -1;
    const tj = j >= 0 ? r.rxs[j]!.t : -1;
    const tk = k >= 0 ? r.apps[k]!.t : -1;
    if (tk >= ti && tk >= tj) {
      const a = r.apps[k--]!;
      if (involves(a.node)) {
        const bad = /timeout|drop|busy|error/.test(a.name) || /drop|limit|fail/i.test(a.detail);
        const cls = bad ? 'bad' : a.name.includes('relay') ? 'relay' : a.name === 'app_rx' || a.name === 'app_ack' ? 'ok' : '';
        out.push({ t: a.t, node: a.node, cls, text: `${a.name} ${a.detail}` });
      }
    } else if (tj >= ti) {
      const x = r.rxs[j--]!;
      if (involves(x.node, x.from)) {
        out.push(
          x.ok
            ? { t: x.t, node: x.node, cls: 'ok', text: `rx from ${x.from}  ${x.rssi} dBm, SNR ${x.snr} dB`, tx: txOfReception(r, x) }
            : { t: x.t, node: x.node, cls: 'bad', text: `lost from ${x.from}: ${LOSS_LABEL[x.reason ?? ''] ?? x.reason}`, tx: txOfReception(r, x) },
        );
      }
    } else {
      const x = r.txs[i--]!;
      if (involves(x.node)) {
        const ms = ((x.end - x.t) / 1000).toFixed(1);
        out.push({ t: x.t, node: x.node, cls: x.relay ? 'relay' : 'tx', text: `tx ${x.len} B, ${ms} ms${x.relay ? ' (relay)' : ''}`, tx: x });
      }
    }
  }
  return out;
}

export function Timeline(props: Props) {
  const { replay, time } = props;
  const events = recentEvents(replay, time, props.selected, 60);
  const step = (dir: 1 | -1) => {
    const t = nextEventTime(replay, time, dir);
    if (t !== undefined) {
      props.onPlay(false);
      props.onSeek(t);
    }
  };

  return (
    <div className="timeline">
      <div className="controls">
        <div className="buttons">
          <button onClick={() => props.onSeek(0)} title="Back to the start">⏮</button>
          <button onClick={() => step(-1)} title="Previous radio event">◀|</button>
          <button className="primary" onClick={() => props.onPlay(!props.playing)} style={{ minWidth: 70 }}>
            {props.playing ? '❚❚ Pause' : '▶ Play'}
          </button>
          <button onClick={() => step(1)} title="Next radio event">|▶</button>
          <span className="clock">
            {formatTime(time)} / {formatTime(replay.duration)}
          </span>
          <label className="hint" htmlFor="speed">speed</label>
          <select id="speed" value={props.speed} onChange={(e) => props.onSpeed(Number(e.target.value))}>
            {SPEEDS.map((s) => (
              <option key={s} value={s}>{s}×</option>
            ))}
          </select>
          {props.selected !== null && <span className="hint">events of node {props.selected}</span>}
        </div>
        <input
          type="range"
          min={0}
          max={replay.duration}
          step={1000}
          value={time}
          onChange={(e) => props.onSeek(Number(e.target.value))}
          aria-label="Virtual time"
        />
        <Activity replay={replay} time={time} onSeek={props.onSeek} />
      </div>
      <div className="events" aria-label="Recent events">
        {events.length === 0 && <div className="empty">no events yet</div>}
        {events.map((e, idx) => (
          <div key={`${e.t}-${idx}`} onClick={() => {
              props.onSeek(e.t);
              props.onSelect(e.node);
              if (e.tx) {
                props.onPacket(e.tx);
              }
            }} title={e.tx ? `${e.text} — click to inspect the frame` : e.text}>
            <span className="t">{formatTime(e.t)}</span>
            <span>{e.node}</span>
            <span className={e.cls}>{e.text}</span>
          </div>
        ))}
      </div>
    </div>
  );
}
