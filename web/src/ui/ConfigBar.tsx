import { useEffect, useState } from 'react';
import type { Config, MetaResponse } from '../api/types';

/** Explorer::spec: players, chips / 10, ante, metas stage, memory stage, then the tag. */
function specOf(config: Config): string {
  return (
    String(
      100000 * config.players +
        100 * config.chips +
        100 * config.ante +
        10 * config.metasStage +
        config.memoryStage,
    ) + config.tag
  );
}

const stageLetter = (stage: number) => String.fromCharCode(65 + stage);

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

/** Steps through a stage axis: A (full detail), B, C, then back to A. */
function Stage({
  label,
  stage,
  count,
  onChange,
}: {
  label: string;
  stage: number;
  count: number;
  onChange: (stage: number) => void;
}) {
  return (
    <button
      type="button"
      onMouseDown={(event) => event.preventDefault()}
      onClick={(event) => {
        release(event);
        onChange((stage + 1) % count);
      }}
      className="rounded-[3px] border px-2 py-[3px] text-[10px] tracking-[0.1em] uppercase"
      style={{
        borderColor: stage > 0 ? 'var(--color-chalk)' : 'var(--color-rule)',
        color: stage > 0 ? 'var(--color-chalk)' : 'var(--color-ash)',
      }}
      title={`${label} stage ${stageLetter(stage)} (A is full detail); click for the next`}
    >
      {label} {stageLetter(stage)}
    </button>
  );
}

/** Which strategy file the page reads: the settings its spec encodes. */
export function ConfigBar({
  meta,
  config,
  onChange,
}: {
  meta: MetaResponse | null;
  config: Config | null;
  onChange: (config: Config) => void;
}) {
  // Typed chips are only committed on Enter or blur, and only on a multiple of 10.
  const [chips, setChips] = useState('');
  useEffect(() => {
    if (config) setChips(String(config.chips));
  }, [config]);

  if (!meta || !config) return null;
  const [minPlayers, maxPlayers] = meta.limits.players;
  const [minChips, maxChips] = meta.limits.chips;
  const [minAnte, maxAnte] = meta.limits.ante;
  const available = new Set(meta.available.map((entry) => entry.spec));
  const spec = specOf(config);
  const exists = available.has(spec);

  function commitChips() {
    const value = Number(chips);
    if (
      config &&
      Number.isInteger(value) &&
      value >= minChips &&
      value % meta!.limits.chipsStep === 0 &&
      value <= maxChips &&
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
          title={`starting stack in chips (blinds 1/2): a multiple of ${meta.limits.chipsStep} from ${minChips} to ${maxChips}`}
        />
      </label>
      <label className="flex items-center gap-2">
        <Label>ante</Label>
        <select
          value={config.ante}
          onChange={(event) => {
            onChange({ ...config, ante: Number(event.target.value) });
            event.currentTarget.blur();
          }}
          className="rounded-[3px] border px-1 py-[2px]"
          style={fieldStyle}
          title="a dead ante in the pot from the deal, paid by no seat"
        >
          {Array.from({ length: maxAnte - minAnte + 1 }, (_, i) => minAnte + i).map((n) => (
            <option key={n} value={n}>
              {n}
            </option>
          ))}
        </select>
      </label>
      <Stage
        label="metas"
        stage={config.metasStage}
        count={meta.limits.metasStages}
        onChange={(stage) => onChange({ ...config, metasStage: stage })}
      />
      <Stage
        label="memory"
        stage={config.memoryStage}
        count={meta.limits.memoryStages}
        onChange={(stage) => onChange({ ...config, memoryStage: stage })}
      />
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
                {entry.spec} · {entry.players} players · {entry.chips} chips · ante {entry.ante} · metas{' '}
                {stageLetter(entry.metasStage)} · memory {stageLetter(entry.memoryStage)} · tag {entry.tag}
              </option>
            ))}
          </select>
        </label>
      ) : null}
    </div>
  );
}
