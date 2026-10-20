/*
 * Copyright 2026 The Kervan Authors
 * SPDX-License-Identifier: Apache-2.0
 */

// A form generated from option specs (a stack's config schema or a PHY's radio settings).
// An empty field means "not set": the stack or device default applies and the key is left out
// of the scenario file.

import { useState } from 'react';

import type { OptionSpec } from '../api';
import { displayValue, parseNumberInput, toHex } from '../model/options';
import type { ConfigValue } from '../model/scenario';

interface Props {
  options: OptionSpec[];
  values: Record<string, ConfigValue> | undefined;
  // shown when a key is not set; falls back to the option's own default
  inherited?: Record<string, ConfigValue | undefined>;
  onChange: (key: string, value: ConfigValue | undefined) => void;
  grouped?: boolean;
  groupPrefix?: string; // e.g. the stack name, so its groups do not look like the PHY's
}

function show(v: ConfigValue | undefined): string {
  if (v === undefined) {
    return '';
  }
  if (typeof v === 'boolean') {
    return v ? 'on' : 'off';
  }
  return String(v);
}

function rangeTitle(o: OptionSpec): string | undefined {
  if (o.min === undefined && o.max === undefined) {
    return undefined;
  }
  const show = (v: number | undefined) =>
    v === undefined ? '' : o.format === 'hex' ? toHex(v) : String(v);
  return `${show(o.min)} … ${show(o.max)}${o.step ? ` in steps of ${o.step}` : ''}`;
}

function NumberField(props: {
  o: OptionSpec;
  value: ConfigValue | undefined;
  placeholder: string;
  onChange: (v: ConfigValue | undefined) => void;
}) {
  const { o, value } = props;
  // keep what the user typed, even if it is not a valid number yet
  const [text, setText] = useState<string | null>(null);
  const shown = text ?? displayValue(o, value);
  return (
    <input
      type="text"
      inputMode={o.format === 'hex' ? 'text' : 'decimal'}
      className={parseNumberInput(o, shown).valid ? undefined : 'invalid'}
      value={shown}
      placeholder={props.placeholder}
      title={rangeTitle(o)}
      onChange={(e) => {
        setText(e.target.value);
        const r = parseNumberInput(o, e.target.value);
        if (r.valid) {
          props.onChange(r.value);
        }
      }}
      onBlur={() => setText(null)}
    />
  );
}

function Field(props: {
  o: OptionSpec;
  value: ConfigValue | undefined;
  inherited: ConfigValue | undefined;
  onChange: (v: ConfigValue | undefined) => void;
}) {
  const { o, value } = props;
  const fallback = props.inherited ?? o.default;
  const placeholder =
    fallback === undefined
      ? 'not set'
      : `default: ${o.type === 'bool' ? show(fallback) : displayValue(o, fallback)}`;

  switch (o.type) {
    case 'bool':
      return (
        <select
          value={value === undefined ? '' : value ? '1' : '0'}
          onChange={(e) =>
            props.onChange(e.target.value === '' ? undefined : e.target.value === '1')
          }
        >
          <option value="">{placeholder}</option>
          <option value="1">on</option>
          <option value="0">off</option>
        </select>
      );
    case 'enum':
      return (
        <select
          value={value === undefined ? '' : String(value)}
          onChange={(e) => {
            const raw = e.target.value;
            const match = o.values?.find((x) => String(x) === raw);
            props.onChange(raw === '' ? undefined : match);
          }}
        >
          <option value="">{placeholder}</option>
          {(o.values ?? []).map((x) => (
            <option key={String(x)} value={String(x)}>
              {String(x)}
            </option>
          ))}
        </select>
      );
    case 'string':
      return (
        <input
          type="text"
          value={value === undefined ? '' : String(value)}
          placeholder={placeholder}
          onChange={(e) => props.onChange(e.target.value === '' ? undefined : e.target.value)}
        />
      );
    default:
      return (
        <NumberField o={o} value={value} placeholder={placeholder} onChange={props.onChange} />
      );
  }
}

export function SchemaForm(props: Props) {
  const values = props.values ?? {};
  const known = new Set(props.options.map((o) => o.key));
  const extra = Object.keys(values).filter((k) => !known.has(k));

  const groups = new Map<string, OptionSpec[]>();
  for (const o of props.options) {
    const g = props.grouped ? (o.group ?? 'other') : '';
    groups.set(g, [...(groups.get(g) ?? []), o]);
  }

  return (
    <div>
      {[...groups].map(([group, options]) => (
        <div key={group}>
          {group && (
            <h3>
              {props.groupPrefix ? `${props.groupPrefix} · ` : ''}
              {group}
            </h3>
          )}
          {options.map((o) => (
            <div className="row" key={o.key}>
              <label title={o.help}>{o.key}</label>
              <Field
                o={o}
                value={values[o.key]}
                inherited={props.inherited?.[o.key]}
                onChange={(v) => props.onChange(o.key, v)}
              />
              {o.help && <div className="help">{o.help}</div>}
            </div>
          ))}
        </div>
      ))}
      {extra.length > 0 && <h3>not in schema</h3>}
      {extra.map((k) => (
        <div className="row" key={k}>
          <label title="unknown to this stack's schema">{k}</label>
          <div className="inline">
            <input
              type="text"
              value={show(values[k])}
              onChange={(e) => props.onChange(k, e.target.value)}
            />
            <button onClick={() => props.onChange(k, undefined)} title="remove">
              ✕
            </button>
          </div>
        </div>
      ))}
    </div>
  );
}

// For stacks without a schema: free key=value pairs.
export function KeyValueForm(props: {
  values: Record<string, ConfigValue> | undefined;
  onChange: (key: string, value: ConfigValue | undefined) => void;
}) {
  const [key, setKey] = useState('');
  const values = props.values ?? {};
  return (
    <div>
      {Object.entries(values).map(([k, v]) => (
        <div className="row" key={k}>
          <label>{k}</label>
          <div className="inline">
            <input
              type="text"
              value={String(v)}
              onChange={(e) => {
                const t = e.target.value;
                const n = Number(t);
                props.onChange(k, t.trim() !== '' && Number.isFinite(n) ? n : t);
              }}
            />
            <button onClick={() => props.onChange(k, undefined)} title="remove">
              ✕
            </button>
          </div>
        </div>
      ))}
      <div className="row">
        <input
          type="text"
          placeholder="new key"
          value={key}
          onChange={(e) => setKey(e.target.value.replace(/[;=\s]/g, ''))}
        />
        <button
          disabled={key === '' || key in values}
          onClick={() => {
            props.onChange(key, '');
            setKey('');
          }}
        >
          Add option
        </button>
      </div>
    </div>
  );
}
