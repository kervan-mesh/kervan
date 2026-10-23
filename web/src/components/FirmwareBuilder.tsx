/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Build the board firmware for the chosen wiring (in Docker, by ksim serve), then open its folder
// or copy it onto a Pico in BOOTSEL mode.

import { useCallback, useEffect, useState } from 'react';

import {
  bootDrives,
  buildFirmware,
  buildStatus,
  flashFirmware,
  revealFirmware,
  type FirmwareBuild,
} from '../api';

interface Props {
  mcu: string;
  module: string;
  pins: Record<string, string>;
  firmware: { name?: string; wiring?: string; error?: string };
  allowed: boolean; // ksim serve --allow-processes
}

function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

export function FirmwareBuilder({ mcu, module, pins, firmware, allowed }: Props) {
  const [status, setStatus] = useState<FirmwareBuild | null>(null);
  const [drives, setDrives] = useState<string[]>([]);
  const [message, setMessage] = useState<string | null>(null);
  const [problem, setProblem] = useState<string | null>(null);

  const refresh = useCallback(async () => {
    try {
      setStatus(await buildStatus());
    } catch {
      setStatus(null); // this ksim serve does not build firmware
    }
  }, []);

  useEffect(() => {
    void refresh();
  }, [refresh]);

  const running = status?.state === 'running';
  const mine = status !== null && status.name === firmware.name;
  const built = mine && status.state === 'done';

  // follow a running build
  useEffect(() => {
    if (!running) {
      return;
    }
    const id = window.setInterval(() => void refresh(), 800);
    return () => window.clearInterval(id);
  }, [running, refresh]);

  // look for a Pico in BOOTSEL mode while there is something to flash
  useEffect(() => {
    if (!built) {
      return;
    }
    let live = true;
    const look = () =>
      bootDrives()
        .then((d) => live && setDrives(d.drives))
        .catch(() => undefined);
    void look();
    const id = window.setInterval(look, 2000);
    return () => {
      live = false;
      window.clearInterval(id);
    };
  }, [built]);

  if (firmware.error) {
    return (
      <div className="firmware">
        <h4>Firmware</h4>
        <p className="hint">{firmware.error}</p>
      </div>
    );
  }

  const build = async () => {
    setMessage(null);
    setProblem(null);
    try {
      setStatus(await buildFirmware(mcu, module, pins));
    } catch (e) {
      setProblem(errorText(e));
    }
  };

  const flash = async (drive?: string) => {
    setMessage(null);
    setProblem(null);
    try {
      const r = await flashFirmware(drive);
      setMessage(`Copied to ${r.drive}: the Pico restarts with the new firmware.`);
    } catch (e) {
      setProblem(errorText(e));
    }
  };

  const log = mine ? status.log : [];
  return (
    <div className="firmware">
      <h4>Firmware</h4>
      <p className="hint">
        For this wiring: <code>{firmware.wiring}</code>
      </p>
      <div className="inline">
        <button
          className="primary"
          style={{ flex: 'none' }}
          onClick={() => void build()}
          disabled={!allowed || running || status === null}
          title={allowed ? 'Build in the dev container (Docker)' : 'Needs ksim serve --allow-processes'}
        >
          {running && mine ? `Building… ${Math.round(status.seconds)} s` : built ? 'Build again' : 'Build firmware'}
        </button>
        {built && (
          <button style={{ flex: 'none' }} onClick={() => void revealFirmware().catch((e) => setProblem(errorText(e)))}>
            Open folder
          </button>
        )}
        {built &&
          (drives.length > 0 ? (
            drives.map((d) => (
              <button key={d} style={{ flex: 'none' }} onClick={() => void flash(d)} title="Copy the .uf2 onto the Pico">
                Flash to Pico ({d})
              </button>
            ))
          ) : (
            <span className="hint" style={{ flex: 'none' }}>
              To flash: hold BOOTSEL while plugging the Pico in.
            </span>
          ))}
      </div>
      {!allowed && (
        <p className="hint">Building runs programs on this computer: start <code>ksim serve --allow-processes</code>.</p>
      )}
      {status !== null && running && !mine && <p className="hint">Another build is running ({status.name}).</p>}
      {built && (
        <p className="hint">
          ✓ {status.file} ({Math.round(status.seconds)} s)
        </p>
      )}
      {mine && status.state === 'failed' && <p className="warning">⚠ {status.error}</p>}
      {problem && <p className="warning">⚠ {problem}</p>}
      {message && <p className="hint">✓ {message}</p>}
      {log.length > 1 && (
        <details open={running}>
          <summary>Build log</summary>
          <pre className="build-log">{log.slice(-40).join('\n')}</pre>
        </details>
      )}
    </div>
  );
}
