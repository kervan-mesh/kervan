/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Client for the `ksim serve` API (docs/web-ui.md).

import type { ScenarioDoc } from './model/scenario';

export type OptionType = 'int' | 'number' | 'bool' | 'string' | 'enum';

export interface OptionSpec {
  key: string;
  type: OptionType;
  help: string;
  min?: number;
  max?: number;
  step?: number;
  values?: (number | string)[];
  default?: number | string | boolean;
  format?: 'hex'; // int shown and stored as "0x..." (addresses)
  group?: string;
}

export interface StackSchema {
  phy: string[];
  options: OptionSpec[];
}

export interface StackInfo {
  name: string;
  version: string;
  schema: StackSchema | null;
}

export type RadioValues = Record<string, number | string | boolean>;

export interface Limits {
  tx_power_dbm?: { min: number; max: number };
  tx_power_levels?: number[];
  freq_hz?: { min: number; max: number };
}

// Hardware catalog (docs/simulator.md, "Hardware"): boards are named MCU + module pairs.
export interface BoardInfo {
  name: string;
  version: number;
  title: string;
  description: string;
  mcu: string;
  module: string;
  integrated: boolean; // radio on the board, nothing to wire
  radio: RadioValues & { type: string };
  limits: Limits;
}

export interface McuInfo {
  name: string;
  version: number;
  title: string;
  description: string;
  chip: string;
  logic_v: number;
  ram_kb: number;
  flash_kb: number;
  clock_mhz: number;
  supply_out_ma: number;
  header: McuHeader | null; // for drawings and pin choices
  io: string[]; // header pins that are GPIOs
  input_only: string[]; // of those, the ones that cannot drive a signal
  spi: { name: string; sck: string[]; mosi: string[]; miso: string[] }[];
  firmware: { project: string; board: string } | null; // the Kervan board firmware builds for it
  notes: string[];
}

// A pin of a header: what the MCU calls it, what is printed next to it, its number (0: none).
export interface HeaderPinInfo {
  name: string;
  label: string;
  number: number;
}

// Pin rows down the left/right edge (top to bottom) or along the bottom (left to right).
export interface HeaderStripInfo {
  name: string;
  side: 'left' | 'right' | 'bottom';
  gap: number;
  pins: HeaderPinInfo[];
}

export interface McuHeader {
  top: string;
  bottom: string;
  strips: HeaderStripInfo[];
}

export interface ModuleInfo {
  name: string;
  version: number;
  title: string;
  description: string;
  chip: string;
  radio: RadioValues & { type: string };
  limits: Limits;
  antenna_dbi: number;
  lna_db: number;
  current_ma: { tx: number; rx: number };
  pins: { name: string; role: string }[];
  header: HeaderPinInfo[][]; // the pads row by row, as on the module
  supply_v: { min: number; max: number } | null;
  notes: string[];
}

export interface Hardware {
  boards: BoardInfo[];
  mcus: McuInfo[];
  modules: ModuleInfo[];
}

export interface WiringRow {
  module_pin: string;
  mcu_pin: string; // "GP18 / SPI0 SCK (pin 24)"
  role: string;
  mcu_name: string; // "GP18"
  mcu_number: number; // number on its connector, 0 if unknown
  mcu_strip: number; // where on the MCU header, -1 if unknown
  mcu_index: number;
}

export interface Wiring {
  integrated: boolean;
  spi?: string;
  rows: WiringRow[];
  warnings: string[];
  notes: string[];
  // with POST /api/wiring: the firmware for this wiring, or why there is none
  firmware?: { name?: string; wiring?: string; error?: string };
}

export interface PhyInfo {
  version: number;
  options: OptionSpec[];
}

export interface ServerInfo {
  ksim: string;
  abi: number;
  scenarios: boolean;
  allow_processes: boolean;
}

export interface ScenarioEntry {
  name: string;
  description?: string;
  nodes?: number;
  error?: string;
}

export interface Link {
  from: number;
  to: number;
  rssi: number;
  snr: number;
}

export interface NodeSummary {
  id: number;
  airtime_us: number;
  tx: number;
  rx_ok: number;
  lost_collision: number;
  lost_half_duplex: number;
  lost_rx_off: number;
}

export interface RunSummary {
  seed: number;
  duration_us: number;
  duty_cycle_limit: number;
  tx: number;
  rx_ok: number;
  lost_collision: number;
  lost_half_duplex: number;
  lost_rx_off: number;
  app_events: Record<string, number>;
  versions: Record<string, string>;
  trace_hash: string;
  nodes: NodeSummary[];
}

export type TraceLine = { t: number; ev: string } & Record<string, unknown>;

export interface RunResult {
  summary: RunSummary;
  truncated: boolean;
  links: Link[];
  trace: TraceLine[];
  hardware?: { node: number; port: string; board: string; id: string; fw: string; version: number }[];
}

export interface Catalog {
  info: ServerInfo;
  stacks: StackInfo[];
  hardware: Hardware;
  phys: Record<string, PhyInfo>;
}

export class ApiError extends Error {}

async function request<T>(method: string, path: string, body?: string): Promise<T> {
  const init: RequestInit = { method };
  if (body !== undefined) {
    init.body = body;
    init.headers = { 'Content-Type': 'application/json' };
  }
  const res = await fetch(path, init);
  const text = await res.text();
  let data: unknown;
  try {
    data = JSON.parse(text);
  } catch {
    throw new ApiError(`${method} ${path}: HTTP ${res.status}: ${text.slice(0, 200)}`);
  }
  if (!res.ok) {
    const msg = (data as { error?: string }).error ?? `HTTP ${res.status}`;
    throw new ApiError(msg);
  }
  return data as T;
}

export async function loadCatalog(): Promise<Catalog> {
  const [info, stacks, hardware, phys] = await Promise.all([
    request<ServerInfo>('GET', '/api/info'),
    request<StackInfo[]>('GET', '/api/stacks'),
    request<Hardware>('GET', '/api/devices'),
    request<Record<string, PhyInfo>>('GET', '/api/phys'),
  ]);
  return { info, stacks, hardware, phys };
}

export const listScenarios = () => request<ScenarioEntry[]>('GET', '/api/scenarios');

export const getScenario = (name: string) =>
  request<ScenarioDoc>('GET', `/api/scenarios/${encodeURIComponent(name)}`);

export const saveScenario = (name: string, text: string) =>
  request<{ ok: true }>('PUT', `/api/scenarios/${encodeURIComponent(name)}`, text);

export const checkScenario = (doc: ScenarioDoc) =>
  request<{ ok: true; links: Link[] }>('POST', '/api/check', JSON.stringify(doc));

export const runScenario = (doc: ScenarioDoc) =>
  request<RunResult>('POST', '/api/run', JSON.stringify(doc));

export interface FrameField {
  name: string;
  value: string;
  offset?: number; // bytes the field covers
  len?: number;
}

// The sending stack's description of a frame; null when it has no decoder or does not
// understand the frame.
export const decodeFrame = (stack: string, config: Record<string, unknown> | undefined, data: string) =>
  request<{ fields: FrameField[] | null }>(
    'POST',
    '/api/decode',
    JSON.stringify({ stack, config: config ?? {}, data }),
  );

// How to connect a board (one name) or an MCU and a module.
export const getWiring = (board: string, module?: string) =>
  request<Wiring>(
    'GET',
    `/api/wiring/${encodeURIComponent(board)}${module ? `/${encodeURIComponent(module)}` : ''}`,
  );

// The wiring for chosen pins (throws with ksim's reason when they cannot work).
export const wiringFor = (mcu: string, module: string, pins: Record<string, string>) =>
  request<Wiring>('POST', '/api/wiring', JSON.stringify({ mcu, module, pins }));

// ---- firmware builds (docs/hardware.md, "Wiring and firmware") ----

export interface FirmwareBuild {
  state: 'idle' | 'running' | 'done' | 'failed';
  name: string;
  wiring: string;
  file: string;
  error: string;
  log: string[];
  seconds: number;
}

export const buildStatus = () => request<FirmwareBuild>('GET', '/api/firmware/build');

export const buildFirmware = (mcu: string, module: string, pins: Record<string, string>) =>
  request<FirmwareBuild>('POST', '/api/firmware/build', JSON.stringify({ mcu, module, pins }));

export const bootDrives = () => request<{ drives: string[] }>('GET', '/api/firmware/drives');

export const flashFirmware = (drive?: string) =>
  request<{ ok: true; drive: string; file: string }>('POST', '/api/firmware/flash', JSON.stringify(drive ? { drive } : {}));

export const revealFirmware = () => request<{ ok: true }>('POST', '/api/firmware/reveal', '{}');

// ---- real boards (docs/hardware.md) ----

export interface BoardHello {
  ev: 'hello';
  fw: string;
  version: number;
  proto: number;
  board: string;
  id: string;
  radio: string;
  wiring?: string; // the pins its firmware was built for
}

export const listPorts = () => request<{ ports: { port: string }[] }>('GET', '/api/ports');

export const boardHello = (port: string) =>
  request<{ hello: BoardHello }>('POST', '/api/ports/hello', JSON.stringify({ port }));

// ---- live sessions (docs/web-ui.md, "Live") ----

export interface LiveStatus {
  state: 'running' | 'paused' | 'done' | 'error';
  t_us: number;
  duration_us: number;
  speed: number;
  hardware?: boolean; // real boards take part: real time only
  total_lines: number;
  truncated: boolean;
  error?: string;
}

export interface LiveMessage extends LiveStatus {
  from: number;
  lines: TraceLine[];
}

export const startLive = (doc: ScenarioDoc, speed: number) =>
  request<LiveStatus>('POST', '/api/live/start', JSON.stringify({ scenario: doc, speed }));

export const controlLive = (action: 'pause' | 'resume' | 'stop' | 'speed', speed?: number) =>
  request<LiveStatus>('POST', '/api/live/control', JSON.stringify({ action, speed }));

export const liveResult = () =>
  request<Omit<RunResult, 'trace'>>('GET', '/api/live/result');

// Server-Sent Events: one message per poll with the state and the new trace lines. Returns a
// function that closes the stream.
export function streamLive(
  from: number,
  onMessage: (m: LiveMessage) => void,
  onError: () => void,
): () => void {
  const es = new EventSource(`/api/live/stream?from=${from}`);
  es.onmessage = (e: MessageEvent<string>) => {
    const m = JSON.parse(e.data) as LiveMessage;
    onMessage(m);
    if (m.state === 'done' || m.state === 'error') {
      es.close();
    }
  };
  es.onerror = () => {
    es.close();
    onError();
  };
  return () => es.close();
}
