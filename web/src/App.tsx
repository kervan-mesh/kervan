/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// ksim web UI: edit a scenario on a map, run it in ksim, replay what happened.
//
// URL parameters (handy for sharing and screenshots): ?scenario=NAME&run=1&t=SECONDS&speed=X
// &node=ID&tab=scenario|node|results&play=0

import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import {
  checkScenario,
  getScenario,
  listScenarios,
  loadCatalog,
  runScenario,
  saveScenario,
  type Catalog,
  type Link,
  type RunResult,
  type ScenarioEntry,
} from './api';
import { Field, type Tool } from './components/Field';
import { NodePanel } from './components/NodePanel';
import { ResultsPanel } from './components/ResultsPanel';
import { ScenarioPanel } from './components/ScenarioPanel';
import { SPEEDS, Timeline } from './components/Timeline';
import { buildReplay, type Replay } from './model/replay';
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

type Tab = 'scenario' | 'node' | 'results';
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

interface Run {
  result: RunResult;
  replay: Replay;
  docText: string; // the scenario it ran, to notice edits afterwards
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
  const [running, setRunning] = useState(false);
  const [selected, setSelected] = useState<number | null>(null);
  const [tab, setTab] = useState<Tab>('scenario');
  const [tool, setTool] = useState<Tool>('select');
  const [showLinks, setShowLinks] = useState(true);
  const [showLosses, setShowLosses] = useState(true);
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
      setRun({ result, replay, docText: JSON.stringify(d) });
      setLinks(result.links);
      setProblem(null);
      setTime(Math.min(opts?.t ?? 0, replay.duration));
      setPlaying(opts?.play ?? true);
      setTab((t) => (t === 'scenario' ? 'results' : t));
    } catch (e) {
      setProblem(errorText(e));
    } finally {
      setRunning(false);
    }
  }, []);

  const open = useCallback(
    async (d: ScenarioDoc, n: string | null) => {
      setDoc(d);
      setName(n);
      setDirty(false);
      setRun(null);
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
        if (params.get('run') === '1') {
          await startRun(d, { t: Number(params.get('t') ?? 0) * 1e6, play: params.get('play') !== '0' });
          if (t) {
            setTab(t as Tab);
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

  // ---- playback ----

  const replay = run?.replay ?? null;
  useEffect(() => {
    if (!playing || !replay) {
      return;
    }
    let last = performance.now();
    let frame = requestAnimationFrame(function tick(now) {
      const dt = now - last;
      last = now;
      let stop = false;
      setTime((t) => {
        const next = t + dt * 1000 * speed;
        if (next >= replay.duration) {
          stop = true;
          return replay.duration;
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
  }, [playing, replay, speed]);

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
  const window_us = Math.max(speed * 250_000, 150_000); // how long an event stays on the map

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
        <button className="primary" onClick={() => void startRun(doc)} disabled={running}>
          {running ? 'Running…' : '▶ Run'}
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
          onSeek={(t) => setTime(Math.max(0, Math.min(t, replay.duration)))}
          onPlay={setPlaying}
          onSpeed={setSpeed}
          onSelect={setSelected}
        />
      ) : (
        <div className="timeline">
          <div className="controls hint">Press ▶ Run to simulate this scenario and replay it here.</div>
        </div>
      )}
    </div>
  );
}
