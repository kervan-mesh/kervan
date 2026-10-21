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
  notes: string[];
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
  supply_v: { min: number; max: number } | null;
  notes: string[];
}

export interface Hardware {
  boards: BoardInfo[];
  mcus: McuInfo[];
  modules: ModuleInfo[];
}

export interface Wiring {
  integrated: boolean;
  rows: { module_pin: string; mcu_pin: string; role: string }[];
  warnings: string[];
  notes: string[];
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
