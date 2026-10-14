import { useEffect, useState } from 'react';
import type { Config, MetaResponse } from '../api/types';

function specOf(config: Config): string {
  return String(
    Math.trunc(
      10000 * config.players +
        10 * config.chips +
        10 * Number(config.metasBasic) +
        Number(config.memoryBasic),
    ),
  );
}

function Label({ children }: { children: React.ReactNode }) {
  return (
    <span className="text-[10px] tracking-[0.14em] uppercase" style={{ color: 'var(--color-ash)' }}>
      {children}
    </span>
  );
}

/**
 * The page is driven from the keyboard, so a control must never keep focus after
 * it is used - otherwise the next keystroke goes to the control instead of the hand.
 */
function release(event: { currentTarget: HTMLElement }) {
  event.currentTarget.blur();
}

function Toggle({ label, on, onChange }: { label: string; on: boolean; onChange: (on: boolean) => void }) {
  return (
    <button
      type="button"
      onMouseDown={(event) => event.preventDefault()}
      onClick={(event) => {
        release(event);
        onChange(!on);
      }}
      className="rounded-[3px] border px-2 py-[3px] text-[10px] tracking-[0.1em] uppercase"
      style={{
        borderColor: on ? 'var(--color-chalk)' : 'var(--color-rule)',
        color: on ? 'var(--color-chalk)' : 'var(--color-ash)',
      }}
      title={`${label} ${on ? 'on' : 'off'}`}
    >
      {label} {on ? 'basic' : 'full'}
    </button>
  );
}

/** Which strategy file the page reads: the four settings its spec encodes. */
export function ConfigBar({
  meta,
  config,
  onChange,
}: {
  meta: MetaResponse | null;
  config: Config | null;
  onChange: (config: Config) => void;
}) {
  // Typed chips are only committed on Enter or blur, and only when even.
  const [chips, setChips] = useState('');
  useEffect(() => {
    if (config) setChips(String(config.chips));
  }, [config]);

  if (!meta || !config) return null;
  const [minPlayers, maxPlayers] = meta.limits.players;
  const available = new Set(meta.available.map((entry) => entry.spec));
  const spec = specOf(config);
  const exists = available.has(spec);

  function commitChips() {
    const value = Number(chips);
    if (
      config &&
      Number.isInteger(value) &&
      value > 0 &&
      value % 2 === 0 &&
      value <= meta!.limits.chipsMax &&
      value !== config.chips
    ) {
      onChange({ ...config, chips: value });
    } else if (config) {
      setChips(String(config.chips));
    }
  }

  const fieldStyle = {
    background: 'var(--color-void)',
    borderColor: 'var(--color-rule)',
    color: 'var(--color-chalk)',
  };

  return (
    <div className="flex items-center gap-4 text-[12px]">
      <label className="flex items-center gap-2">
        <Label>players</Label>
        <select
          value={config.players}
          onChange={(event) => {
            onChange({ ...config, players: Number(event.target.value) });
            event.currentTarget.blur();
          }}
          className="rounded-[3px] border px-1 py-[2px]"
          style={fieldStyle}
        >
          {Array.from({ length: maxPlayers - minPlayers + 1 }, (_, i) => minPlayers + i).map((n) => (
            <option key={n} value={n}>
              {n}
            </option>
          ))}
        </select>
      </label>
      <label className="flex items-center gap-2">
        <Label>chips</Label>
        <input
          value={chips}
          inputMode="numeric"
          onChange={(event) => setChips(event.target.value.replace(/[^0-9]/g, ''))}
          onKeyDown={(event) => {
            if (event.key === 'Enter') event.currentTarget.blur();
            if (event.key === 'Escape') {
              setChips(String(config.chips));
              event.currentTarget.blur();
            }
          }}
          onBlur={commitChips}
          className="w-16 rounded-[3px] border px-1 py-[2px]"
          style={fieldStyle}
          title="starting stack in chips (blinds 1/2); must be even"
        />
      </label>
      <Toggle label="metas" on={config.metasBasic} onChange={(on) => onChange({ ...config, metasBasic: on })} />
      <Toggle label="memory" on={config.memoryBasic} onChange={(on) => onChange({ ...config, memoryBasic: on })} />
      <span className="flex items-center gap-2">
        <Label>spec</Label>
        <span style={{ color: exists ? 'var(--color-chalk)' : 'var(--color-act-big)' }}>
          {spec}
          {exists ? '' : ' · not trained'}
        </span>
      </span>
      {meta.available.length > 0 ? (
        <label className="flex items-center gap-2">
          <Label>trained</Label>
          <select
            value={exists ? spec : ''}
            onChange={(event) => {
              const chosen = meta.available.find((entry) => entry.spec === event.target.value);
              if (chosen) {
                const { spec: _spec, ...next } = chosen;
                onChange(next);
              }
              event.currentTarget.blur();
            }}
            className="max-w-[22rem] rounded-[3px] border px-1 py-[2px]"
            style={fieldStyle}
          >
            {exists ? null : <option value="">—</option>}
            {meta.available.map((entry) => (
              <option key={entry.spec} value={entry.spec}>
                {entry.spec} · {entry.players} players · {entry.chips} chips · metas{' '}
                {entry.metasBasic ? 'basic' : 'full'} · memory {entry.memoryBasic ? 'basic' : 'full'}
              </option>
            ))}
          </select>
        </label>
      ) : null}
    </div>
  );
}
