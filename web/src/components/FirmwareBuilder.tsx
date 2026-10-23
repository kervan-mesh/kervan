/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// Build the board firmware for the chosen wiring (in Docker, by ksim serve), then open its folder
// and put it on the board: copied onto the board's USB drive (Pico in BOOTSEL, Nucleo's ST-LINK,
// Nordic DK's J-Link) or written over the serial port with esptool (ESP32).

import { useCallback, useEffect, useState } from 'react';

import {
  bootDrives,
  buildFirmware,
  buildStatus,
  flashFirmware,
  listPorts,
  revealFirmware,
  type BootDrive,
  type DriveKind,
  type FirmwareBuild,
  type FirmwareInfo,
} from '../api';

interface Props {
  mcu: string;
  module: string;
  pins: Record<string, string>;
  firmware: FirmwareInfo;
  allowed: boolean; // ksim serve --allow-processes
  port?: string; // the node's real-board port, a good guess for esptool
}

const DRIVE_HINT: Record<DriveKind, string> = {
  rp2: 'To flash: hold BOOTSEL while plugging the Pico in (drive RPI-RP2).',
  stlink: 'To flash: plug in the board’s ST-LINK USB (drive NODE_…).',
  jlink: 'To flash: plug in the DK’s USB (drive JLINK).',
};

function errorText(e: unknown): string {
  return e instanceof Error ? e.message : String(e);
}

export function FirmwareBuilder({ mcu, module, pins, firmware, allowed, port }: Props) {
  const [status, setStatus] = useState<FirmwareBuild | null>(null);
  const [drives, setDrives] = useState<BootDrive[]>([]);
  const [ports, setPorts] = useState<string[]>([]);
  const [espPort, setEspPort] = useState(port ?? '');
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
  const built = mine && status.file !== '' && !(running && status.job === 'build');
  const esptool = firmware.flash === 'esptool';

  // follow a running job
  useEffect(() => {
    if (!running) {
      return;
    }
    const id = window.setInterval(() => void refresh(), 800);
    return () => window.clearInterval(id);
  }, [running, refresh]);

  // look for the board's drive, or the serial ports, while there is something to flash
  useEffect(() => {
    if (!built) {
      return;
    }
    let live = true;
    const look = () => {
      if (esptool) {
        listPorts()
          .then((p) => live && setPorts(p.ports.map((x) => x.port)))
          .catch(() => undefined);
      } else {
        bootDrives()
          .then((d) => live && setDrives(d.drives.filter((x) => x.kind === firmware.drive_kind)))
          .catch(() => undefined);
      }
    };
    look();
    const id = window.setInterval(look, esptool ? 5000 : 2000);
    return () => {
      live = false;
      window.clearInterval(id);
    };
  }, [built, esptool, firmware.drive_kind]);

  useEffect(() => {
    if (!espPort && ports.length > 0) {
      setEspPort(port && ports.includes(port) ? port : ports[0]!);
    }
  }, [ports, port, espPort]);

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

  const flash = async (target: { drive: string } | { port: string }) => {
    setMessage(null);
    setProblem(null);
    try {
      const r = await flashFirmware(target);
      if (r.drive) {
        setMessage(`Copied to ${r.drive}: the board restarts with the new firmware.`);
      } else {
        await refresh(); // esptool runs: follow it
      }
    } catch (e) {
      setProblem(errorText(e));
    }
  };

  const log = mine ? status.log : [];
  const sdk = firmware.board ? ` (${firmware.output} for ${firmware.board})` : '';
  return (
    <div className="firmware">
      <h4>Firmware</h4>
      <p className="hint">
        For this wiring: <code>{firmware.wiring}</code>
        {sdk}
      </p>
      <div className="inline wrap">
        <button
          className="primary"
          style={{ flex: 'none' }}
          onClick={() => void build()}
          disabled={!allowed || running || status === null}
          title={allowed ? 'Build in the dev container (Docker)' : 'Needs ksim serve --allow-processes'}
        >
          {running && mine
            ? `${status.job === 'flash' ? 'Flashing' : 'Building'}… ${Math.round(status.seconds)} s`
            : built
              ? 'Build again'
              : 'Build firmware'}
        </button>
        {built && (
          <button style={{ flex: 'none' }} onClick={() => void revealFirmware().catch((e) => setProblem(errorText(e)))}>
            Open folder
          </button>
        )}
        {built &&
          !esptool &&
          drives.map((d) => (
            <button key={d.path} style={{ flex: 'none' }} onClick={() => void flash({ drive: d.path })} title="Copy the firmware onto the board">
              Flash to {d.label} ({d.path})
            </button>
          ))}
        {built && esptool && (
          <>
            <select value={espPort} onChange={(e) => setEspPort(e.target.value)} style={{ flex: 'none' }} aria-label="Serial port for esptool">
              {ports.length === 0 && <option value="">no serial ports</option>}
              {ports.map((p) => (
                <option key={p} value={p}>
                  {p}
                </option>
              ))}
            </select>
            <button
              style={{ flex: 'none' }}
              disabled={!espPort || running}
              onClick={() => void flash({ port: espPort })}
              title="Write the firmware with esptool (pip install esptool)"
            >
              Flash over USB
            </button>
          </>
        )}
      </div>
      {built && !esptool && drives.length === 0 && firmware.drive_kind && (
        <p className="hint">{DRIVE_HINT[firmware.drive_kind]}</p>
      )}
      {built && esptool && (
        <p className="hint">
          Writes with esptool on this computer (<code>pip install esptool</code>); close live
          sessions and serial monitors on that port first.
        </p>
      )}
      {!allowed && (
        <p className="hint">
          Building runs programs on this computer: start <code>ksim serve --allow-processes</code>.
        </p>
      )}
      {status !== null && running && !mine && <p className="hint">Another job is running ({status.name}).</p>}
      {built && status.job === 'build' && status.state === 'done' && (
        <p className="hint">
          ✓ {status.file} ({Math.round(status.seconds)} s)
        </p>
      )}
      {mine && status.job === 'flash' && status.state === 'done' && <p className="hint">✓ Written to the board.</p>}
      {mine && status.state === 'failed' && <p className="warning">⚠ {status.error}</p>}
      {problem && <p className="warning">⚠ {problem}</p>}
      {message && <p className="hint">✓ {message}</p>}
      {log.length > 1 && (
        <details open={running}>
          <summary>{status?.job === 'flash' ? 'Flash log' : 'Build log'}</summary>
          <pre className="build-log">{log.slice(-40).join('\n')}</pre>
        </details>
      )}
    </div>
  );
}
