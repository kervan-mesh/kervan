/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// ksim web UI: edit a scenario on a map, run it in ksim, replay what happened.
//
// URL parameters (handy for sharing and screenshots): ?scenario=NAME&run=1&t=SECONDS&speed=X
// &node=ID&tab=scenario|node|results|packet&play=0&frame=NODE@SECONDS (inspect that frame)
// &live=SPEED (start a live session at SPEED x real time)

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import {
  checkScenario,
  controlLive,
  getScenario,
  listScenarios,
  liveResult,
  loadCatalog,
  runScenario,
  saveScenario,
  startLive,
  streamLive,
  type Catalog,
  type Link,
  type LiveStatus,
  type RunResult,
  type ScenarioEntry,
} from './api';
import { Field, type Tool } from './components/Field';
import { NodePanel } from './components/NodePanel';
import { PacketPanel } from './components/PacketPanel';
import { ResultsPanel } from './components/ResultsPanel';
import { ScenarioPanel } from './components/ScenarioPanel';
import { SPEEDS, Timeline } from './components/Timeline';
import {
  appendTrace,
  buildReplay,
  emptyReplay,
  receptionsOf,
  type Replay,
  type Tx,
} from './model/replay';
import {
  addNode,
  findNode,
  formatScenario,
  moveNode,
  newScenario,
  removeNode,
  updateNode,
  type NodeDoc,
  type ScenarioDoc,
} from './model/scenario';

type Tab = 'scenario' | 'node' | 'results' | 'packet';
type Theme = 'auto' | 'light' | 'dark';

const params = new URLSearchParams(window.location.search);
const NAME_RE = /^[A-Za-z0-9_-]{1,64}$/;

function stored(key: string): string | null {
  try {
    return window.localStorage.getItem(key);
  } catch {
    return null;
  }
}

function store(key: string, value: string) {
  try {
    window.localStorage.setItem(key, value);
  } catch {
    // private mode: nothing to remember
  }
}

function applyTheme(theme: Theme) {
  if (theme === 'auto') {
    delete document.documentElement.dataset.theme;
  } else {
    document.documentElement.dataset.theme = theme;
  }
}

function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

// A live session in progress (POST /api/live/start, streamed over Server-Sent Events).
interface Live {
  replay: Replay; // grows with every message
  doc: ScenarioDoc;
  docText: string;
  status: LiveStatus;
  headAt: number; // performance.now() when status.t_us arrived
  received: number; // trace lines so far
  version: number; // bumps with every message, for memoized views
}

const LIVE_SPEEDS = [1, 10, 100, 1000];

interface Run {
  result: RunResult;
  replay: Replay;
  docText: string; // the scenario it ran, to notice edits afterwards
  doc: ScenarioDoc; // the same, parsed: stacks and configs of the senders
}

export function App() {
  const [catalog, setCatalog] = useState<Catalog | null>(null);
  const [scenarios, setScenarios] = useState<ScenarioEntry[]>([]);
  const [name, setName] = useState<string | null>(null);
  const [doc, setDoc] = useState<ScenarioDoc>(() => newScenario());
  const [dirty, setDirty] = useState(false);
  const [links, setLinks] = useState<Link[]>([]);
  const [problem, setProblem] = useState<string | null>(null);
  const [fatal, setFatal] = useState<string | null>(null);
  const [run, setRun] = useState<Run | null>(null);
  const [live, setLive] = useState<Live | null>(null);
  const [follow, setFollow] = useState(true); // the view keeps up with a live session
  const closeStream = useRef<(() => void) | null>(null);
  // startLiveRun is defined further down; the URL handler at mount reaches it through this
  const startLiveRef = useRef<((d: ScenarioDoc, speed: number) => Promise<void>) | null>(null);
  const [running, setRunning] = useState(false);
  const [selected, setSelected] = useState<number | null>(null);
  const [packet, setPacket] = useState<Tx | null>(null); // frame open in the inspector
  const [tab, setTab] = useState<Tab>('scenario');
  const [tool, setTool] = useState<Tool>('select');
  const [showLinks, setShowLinks] = useState(true);
  const [showLosses, setShowLosses] = useState(true);
  const [showDistances, setShowDistances] = useState(() => stored('ksim.distances') !== '0');
  const [fitKey, setFitKey] = useState(0);
  const [time, setTime] = useState(0);
  const [playing, setPlaying] = useState(false);
  const [speed, setSpeed] = useState(() => {
    const s = Number(params.get('speed'));
    return SPEEDS.includes(s) ? s : 10;
  });
  const [theme, setTheme] = useState<Theme>(() => (stored('ksim.theme') as Theme) ?? 'auto');
  const fileInput = useRef<HTMLInputElement>(null);

  useEffect(() => applyTheme(theme), [theme]);

  // ---- loading ----

  const check = useCallback(async (d: ScenarioDoc) => {
    try {
      const r = await checkScenario(d);
      setLinks(r.links);
      setProblem(null);
    } catch (e) {
      setProblem(errorText(e));
    }
  }, []);

  const startRun = useCallback(async (d: ScenarioDoc, opts?: { t?: number; play?: boolean }) => {
    setRunning(true);
    setPlaying(false);
    try {
      const result = await runScenario(d);
      const replay = buildReplay(result.trace, result.summary.duration_us);
      setRun({ result, replay, docText: JSON.stringify(d), doc: d });
      setPacket(null);
      setLinks(result.links);
      setProblem(null);
      setTime(Math.min(opts?.t ?? 0, replay.duration));
      setPlaying(opts?.play ?? true);
      setTab((t) => (t === 'scenario' ? 'results' : t));
      return replay;
    } catch (e) {
      setProblem(errorText(e));
      return null;
    } finally {
      setRunning(false);
    }
  }, []);

  const open = useCallback(
    async (d: ScenarioDoc, n: string | null) => {
      setDoc(d);
      setName(n);
      setDirty(false);
      closeStream.current?.();
      setLive((l) => {
        if (l && (l.status.state === 'running' || l.status.state === 'paused')) {
          void controlLive('stop').catch(() => undefined);
        }
        return null;
      });
      setRun(null);
      setPacket(null);
      setSelected(null);
      setTime(0);
      setPlaying(false);
      setFitKey((k) => k + 1);
      setShowLinks(d.nodes.length <= 20); // links clutter dense networks
      if (n) {
        store('ksim.scenario', n);
      }
      await check(d);
    },
    [check],
  );

  useEffect(() => {
    (async () => {
      try {
        const [cat, list] = await Promise.all([loadCatalog(), listScenarios()]);
        setCatalog(cat);
        setScenarios(list);
        const wanted = params.get('scenario') ?? stored('ksim.scenario') ?? 'line_5';
        const pick = list.find((s) => s.name === wanted && !s.error) ?? list.find((s) => !s.error);
        const d = pick ? await getScenario(pick.name) : newScenario();
        await open(d, pick?.name ?? null);
        const node = Number(params.get('node'));
        if (params.has('node') && d.nodes.some((n) => n.id === node)) {
          setSelected(node);
        }
        const t = params.get('tab');
        if (t === 'scenario' || t === 'node' || t === 'results') {
          setTab(t);
        }
        if (params.has('live')) {
          await startLiveRef.current?.(d, Number(params.get('live')) || 10);
        } else if (params.get('run') === '1') {
          const replay = await startRun(d, {
            t: Number(params.get('t') ?? 0) * 1e6,
            play: params.get('play') !== '0',
          });
          if (t) {
            setTab(t as Tab);
          }
          // frame=NODE@SECONDS: that node's first frame at or after that time, in the inspector
          const frame = /^(\d+)@([\d.]+)$/.exec(params.get('frame') ?? '');
          const tx =
            frame &&
            replay?.txs.find((x) => x.node === Number(frame[1]) && x.t >= Number(frame[2]) * 1e6);
          if (tx) {
            setPacket(tx);
            setTab('packet');
          }
        }
      } catch (e) {
        setFatal(`Cannot reach ksim serve: ${errorText(e)}`);
      }
    })();
  }, [open, startRun]);

  // ---- editing ----

  const checkTimer = useRef<number | undefined>(undefined);
  const edit = useCallback(
    (d: ScenarioDoc, recheck = true) => {
      setDoc(d);
      setDirty(true);
      if (recheck) {
        window.clearTimeout(checkTimer.current);
        checkTimer.current = window.setTimeout(() => void check(d), 250);
      }
    },
    [check],
  );

  const select = useCallback((id: number | null) => {
    setSelected(id);
    if (id !== null) {
      setTab((t) => (t === 'results' ? t : 'node'));
    }
  }, []);

  const deleteSelected = useCallback(() => {
    if (selected !== null) {
      edit(removeNode(doc, selected));
      setSelected(null);
    }
  }, [doc, edit, selected]);

  // ---- live sessions ----

  const finishLive = useCallback(async (l: Live) => {
    try {
      const r = await liveResult();
      setRun({ result: { ...r, trace: [] }, replay: l.replay, docText: l.docText, doc: l.doc });
      setLinks(r.links);
      setTab((t) => (t === 'scenario' ? 'results' : t));
    } catch (e) {
      setProblem(errorText(e));
    }
    setLive(null);
    setPlaying(false);
  }, []);

  const startLiveRun = useCallback(
    async (d: ScenarioDoc, liveSpeed: number) => {
      closeStream.current?.();
      setPlaying(false);
      try {
        const status = await startLive(d, liveSpeed);
        const first: Live = {
          replay: emptyReplay(status.duration_us),
          doc: d,
          docText: JSON.stringify(d),
          status,
          headAt: performance.now(),
          received: 0,
          version: 0,
        };
        setRun(null);
        setPacket(null);
        setLive(first);
        setFollow(true);
        setTime(0);
        setProblem(null);
        let current = first;
        closeStream.current = streamLive(
          0,
          (m) => {
            appendTrace(current.replay, m.lines);
            current = {
              ...current,
              status: m,
              headAt: performance.now(),
              received: m.from + m.lines.length,
              version: current.version + 1,
            };
            setLive(current);
            if (m.state === 'done') {
              void finishLive(current);
            } else if (m.state === 'error') {
              setProblem(m.error ?? 'the live session failed');
            }
          },
          () => setProblem('The live stream was interrupted.'),
        );
      } catch (e) {
        setProblem(errorText(e));
      }
    },
    [finishLive],
  );

  startLiveRef.current = startLiveRun;

  const liveControl = useCallback(
    async (action: 'pause' | 'resume' | 'stop' | 'speed', v?: number) => {
      try {
        const status = await controlLive(action, v);
        setLive((l) => (l ? { ...l, status, headAt: performance.now() } : l));
      } catch (e) {
        setProblem(errorText(e));
      }
    },
    [],
  );

  useEffect(() => () => closeStream.current?.(), []);

  // following a live session: the clock runs at the session's speed, never far past what arrived
  useEffect(() => {
    if (!live || !follow) {
      return;
    }
    const { status, headAt } = live;
    if (status.state !== 'running') {
      setTime(Math.min(status.t_us, status.duration_us));
      return;
    }
    let frame = requestAnimationFrame(function tick(now) {
      const ahead = Math.min((now - headAt) * 1000 * status.speed, 250_000 * status.speed);
      setTime(Math.min(status.t_us + ahead, status.duration_us));
      frame = requestAnimationFrame(tick);
    });
    return () => cancelAnimationFrame(frame);
  }, [live, follow]);

  // ---- playback ----

  const replay = live?.replay ?? run?.replay ?? null;
  const playLimit = live ? live.status.t_us : (replay?.duration ?? 0);
  useEffect(() => {
    if (!playing || !replay || (live && follow)) {
      return;
    }
    let last = performance.now();
    let frame = requestAnimationFrame(function tick(now) {
      const dt = now - last;
      last = now;
      let stop = false;
      setTime((t) => {
        const next = t + dt * 1000 * speed;
        if (next >= playLimit) {
          stop = true;
          return playLimit;
        }
        return next;
      });
      if (stop) {
        setPlaying(false);
      } else {
        frame = requestAnimationFrame(tick);
      }
    });
    return () => cancelAnimationFrame(frame);
  }, [playing, replay, speed, playLimit, live, follow]);

  // keyboard: space play/pause, Delete removes the node, Escape leaves add mode
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const target = e.target as HTMLElement;
      if (['INPUT', 'TEXTAREA', 'SELECT'].includes(target.tagName)) {
        return;
      }
      if (e.key === ' ' && replay) {
        e.preventDefault();
        setPlaying((p) => !p);
      } else if (e.key === 'Delete' || e.key === 'Backspace') {
        deleteSelected();
      } else if (e.key === 'Escape') {
        setTool('select');
        setSelected(null);
      }
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  }, [replay, deleteSelected]);

  // ---- files ----

  const saveAs = async () => {
    const suggestion = name ?? 'my_scenario';
    const n = window.prompt('Save scenario as (letters, digits, _ and -):', suggestion);
    if (n === null) {
      return;
    }
    if (!NAME_RE.test(n)) {
      setProblem(`“${n}” is not a valid name: use letters, digits, _ and -`);
      return;
    }
    if (n !== name && scenarios.some((s) => s.name === n) && !window.confirm(`Replace ${n}.json?`)) {
      return;
    }
    await save(n);
  };

  const save = async (n: string) => {
    try {
      await saveScenario(n, formatScenario(doc));
      setName(n);
      setDirty(false);
      store('ksim.scenario', n);
      setScenarios(await listScenarios());
    } catch (e) {
      setProblem(errorText(e));
    }
  };

  const download = () => {
    const blob = new Blob([formatScenario(doc)], { type: 'application/json' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = `${name ?? 'scenario'}.json`;
    a.click();
    URL.revokeObjectURL(a.href);
  };

  const openFile = async (file: File) => {
    try {
      const d = JSON.parse(await file.text()) as ScenarioDoc;
      if (!Array.isArray(d.nodes)) {
        throw new Error('not a scenario: "nodes" is missing');
      }
      await open(d, null);
      setDirty(true);
    } catch (e) {
      setProblem(`${file.name}: ${errorText(e)}`);
    }
  };

  const confirmDiscard = () => !dirty || window.confirm('Discard unsaved changes?');

  // ---- derived ----

  const stale = run !== null && run.docText !== JSON.stringify(doc);
  const node = selected !== null ? findNode(doc, selected) : undefined;
  const nodeStats = run?.result.summary.nodes.find((n) => n.id === selected);
  const viewSpeed = live && follow ? live.status.speed : speed;
  const window_us = Math.max(viewSpeed * 250_000, 150_000); // how long an event stays on the map
  const liveActive =
    live !== null && (live.status.state === 'running' || live.status.state === 'paused');

  const notes = useMemo(() => {
    const m = new Map<number, string>();
    if (run) {
      const s = run.result.summary;
      for (const n of s.nodes) {
        m.set(n.id, `${((100 * n.airtime_us) / Math.max(1, s.duration_us)).toFixed(2)} %`);
      }
    }
    return m;
  }, [run]);

  if (fatal) {
    return (
      <div className="empty" style={{ marginTop: '20vh' }}>
        <h2>ksim</h2>
        <p>{fatal}</p>
        <p>Start it with <code>ksim serve</code> and reload this page.</p>
      </div>
    );
  }
  if (!catalog) {
    return <div className="empty" style={{ marginTop: '20vh' }}>Loading…</div>;
  }

  return (
    <div className="app">
      <header className="topbar">
        <span className="brand">
          ksim<small>v{catalog.info.ksim}</small>
        </span>
        <select
          value={name ?? ''}
          aria-label="Scenario"
          onChange={async (e) => {
            if (!confirmDiscard()) {
              return;
            }
            try {
              await open(await getScenario(e.target.value), e.target.value);
            } catch (err) {
              setProblem(errorText(err));
            }
          }}
        >
          {name === null && <option value="">(unsaved)</option>}
          {scenarios.map((s) => (
            <option key={s.name} value={s.name} disabled={!!s.error} title={s.error ?? s.description}>
              {s.name} ({s.nodes ?? '?'} nodes)
            </option>
          ))}
        </select>
        {dirty && <span className="dirty" title="unsaved changes">●</span>}
        <button onClick={() => confirmDiscard() && void open(newScenario(), null)}>New</button>
        <button onClick={() => fileInput.current?.click()}>Open file…</button>
        <input
          ref={fileInput}
          type="file"
          accept=".json,application/json"
          hidden
          onChange={(e) => {
            const f = e.target.files?.[0];
            e.target.value = '';
            if (f && confirmDiscard()) {
              void openFile(f);
            }
          }}
        />
        <button onClick={() => (name ? void save(name) : void saveAs())} disabled={!catalog.info.scenarios}>
          Save
        </button>
        <button onClick={() => void saveAs()} disabled={!catalog.info.scenarios}>Save as…</button>
        <button onClick={download}>Download</button>
        <span className="spacer" />
        <span className="status">
          {doc.nodes.length} nodes · {doc.duration_s ?? 60} s simulated
        </span>
        <button
          className="primary"
          onClick={() => void startRun(doc)}
          disabled={running || liveActive}
        >
          {running ? 'Running…' : '▶ Run'}
        </button>
        <button
          className="live-button"
          onClick={() => void startLiveRun(doc, 10)}
          disabled={running || liveActive}
          title="Run in real time (10x to start) and watch it as it happens"
        >
          ● Live
        </button>
        <button
          title="Theme: auto / light / dark"
          onClick={() => {
            const next: Theme = theme === 'auto' ? 'light' : theme === 'light' ? 'dark' : 'auto';
            setTheme(next);
            store('ksim.theme', next);
          }}
        >
          {theme === 'auto' ? '◐' : theme === 'light' ? '☀' : '☾'}
        </button>
      </header>

      <div className="main">
        <Field
          doc={doc}
          links={links}
          showLinks={showLinks}
          showLosses={showLosses}
          showDistances={showDistances}
          packet={packet && replay ? { tx: packet, rxs: receptionsOf(replay, packet) } : null}
          replay={replay}
          time={time}
          window={window_us}
          selected={selected}
          tool={tool}
          fitKey={fitKey}
          nodeNotes={notes}
          onSelect={select}
          onMove={(id, x, y, done) => edit(moveNode(doc, id, x, y), done)}
          onAdd={(x, y) => {
            const next = addNode(doc, x, y, node ?? doc.nodes[doc.nodes.length - 1]);
            edit(next);
            setSelected(next.nodes[next.nodes.length - 1]!.id);
            setTab('node');
          }}
        >
          <div className="field-tools">
            <button className={tool === 'select' ? 'active' : undefined} onClick={() => setTool('select')}>
              Select / move
            </button>
            <button
              className={tool === 'add' ? 'active' : undefined}
              onClick={() => setTool(tool === 'add' ? 'select' : 'add')}
              title="Click the map to place nodes like the selected one (Esc to stop)"
            >
              + Add node
            </button>
            <button onClick={() => setFitKey((k) => k + 1)}>Fit</button>
            <button className={showLinks ? 'active' : undefined} onClick={() => setShowLinks(!showLinks)}>
              Links
            </button>
            <button
              className={showLosses ? 'active' : undefined}
              onClick={() => setShowLosses(!showLosses)}
              title="Show frames that were heard but lost"
            >
              Losses
            </button>
            <button
              className={showDistances && showLinks ? 'active' : undefined}
              disabled={!showLinks}
              onClick={() => {
                setShowDistances(!showDistances);
                store('ksim.distances', showDistances ? '0' : '1');
              }}
              title="Distance between linked nodes (needs Links)"
            >
              Distances
            </button>
          </div>
          {problem && <div className="banner error">{problem}</div>}
          {!problem && stale && (
            <div className="banner">
              The scenario changed after this run. <button onClick={() => void startRun(doc)}>Run again</button>
            </div>
          )}
          <div className="field-legend">
            {showLinks && (
              <span title="Who can hear whom without interference (solid: both ways, dashed: one way)">
                <span className="swatch" style={{ background: 'var(--link-good)' }} />
                link
              </span>
            )}
            {replay && (
              <>
                <span><span className="swatch" style={{ background: 'var(--tx)' }} />sending</span>
                <span><span className="swatch" style={{ background: 'var(--relay)' }} />relay</span>
                <span><span className="swatch" style={{ background: 'var(--rx-ok)' }} />received</span>
                <span><span className="swatch" style={{ background: 'var(--lost-collision)' }} />collision</span>
                <span><span className="swatch" style={{ background: 'var(--lost-half)' }} />half-duplex</span>
                <span><span className="swatch" style={{ background: 'var(--lost-off)' }} />rx off</span>
              </>
            )}
          </div>
        </Field>

        <aside className="sidebar">
          <nav className="tabs">
            <button className={tab === 'scenario' ? 'active' : undefined} onClick={() => setTab('scenario')}>
              Scenario
            </button>
            <button className={tab === 'node' ? 'active' : undefined} onClick={() => setTab('node')}>
              Node{selected !== null ? ` ${selected}` : ''}
            </button>
            <button
              className={tab === 'results' ? 'active' : undefined}
              onClick={() => setTab('results')}
              disabled={!run}
            >
              Results
            </button>
            <button
              className={tab === 'packet' ? 'active' : undefined}
              onClick={() => setTab('packet')}
              disabled={!replay}
              title="Click a frame in the event list to inspect it"
            >
              Packet
            </button>
          </nav>
          {tab === 'scenario' && (
            <ScenarioPanel doc={doc} catalog={catalog} selected={selected} onChange={edit} onSelect={select} />
          )}
          {tab === 'node' &&
            (node ? (
              <NodePanel
                doc={doc}
                node={node}
                catalog={catalog}
                stats={nodeStats}
                replay={replay}
                durationUs={run?.result.summary.duration_us ?? 0}
                onChange={(n: NodeDoc) => edit(updateNode(doc, n.id, () => n))}
                onRename={(id) => {
                  if (doc.nodes.some((n) => n.id === id)) {
                    setProblem(`node id ${id} is taken`);
                    return;
                  }
                  edit(updateNode(doc, node.id, (n) => ({ ...n, id })));
                  setSelected(id);
                }}
                onDelete={deleteSelected}
              />
            ) : (
              <div className="empty">Select a node on the map.</div>
            ))}
          {tab === 'packet' &&
            replay &&
            (packet ? (
              <PacketPanel
                replay={replay}
                tx={packet}
                doc={(live ?? run)!.doc}
                onSeek={(t) => {
                  setPlaying(false);
                  setTime(t);
                }}
                onSelectNode={setSelected}
              />
            ) : (
              <div className="empty">Click a frame (tx, rx or lost) in the event list below.</div>
            ))}
          {tab === 'results' && run && (
            <ResultsPanel result={run.result} replay={run.replay} selected={selected} onSelect={setSelected} />
          )}
        </aside>
      </div>

      {replay ? (
        <Timeline
          replay={replay}
          time={time}
          playing={playing}
          speed={speed}
          selected={selected}
          onSeek={(t) => {
            setFollow(false);
            setTime(Math.max(0, Math.min(t, playLimit)));
          }}
          version={live?.version ?? 0}
          live={
            live
              ? {
                  status: live.status,
                  follow,
                  speeds: LIVE_SPEEDS,
                  onFollow: () => setFollow(true),
                  onPause: () => void liveControl('pause'),
                  onResume: () => void liveControl('resume'),
                  onStop: () => void liveControl('stop'),
                  onSpeed: (v: number) => void liveControl('speed', v),
                }
              : undefined
          }
          onPlay={setPlaying}
          onSpeed={setSpeed}
          onSelect={setSelected}
          onPacket={(tx) => {
            setPacket(tx);
            setTab('packet');
          }}
        />
      ) : (
        <div className="timeline">
          <div className="controls hint">Press ▶ Run to simulate this scenario and replay it here.</div>
        </div>
      )}
    </div>
  );
}
