import { useMemo } from 'react';
import type { Combo, Counts } from '../api/types';
import type { HandState } from '../hand/useHand';
import { OPTION_COLORS, OPTION_KEYS, OPTION_NAMES } from '../hand/seats';
import { CardRow } from './Card';
import { Keycap } from './Keycap';

const TEMP_MIN = 0;
const TEMP_MAX = 12;

/** Mean visits per *possible* holding: the 1326 less the rows the board rules out. */
function averageVisits(combos: Combo[] | undefined, counts: Counts | undefined): number | null {
  if (!combos || !counts) return null;
  const possible = 1326 - counts.invalid;
  if (possible <= 0) return null;
  let total = 0;
  for (const row of combos) {
    if (row?.found && typeof row.visits === 'number') total += row.visits;
  }
  return total / possible;
}

function Spec({ label, children }: { label: string; children: React.ReactNode }) {
  return (
    <div className="flex items-center justify-between gap-3 py-[3px]">
      <span className="text-[10px] tracking-[0.14em] uppercase" style={{ color: 'var(--color-ash)' }}>
        {label}
      </span>
      <span className="text-[12px]">{children}</span>
    </div>
  );
}

export function ActionColumn(state: HandState & { setInvTemp: (value: number) => void }) {
  const { frame, pending, mode, ledger, seatNames, invTemp, error, loading } = state;
  const node = frame?.response.node;
  const transition = frame?.response.transition;

  const average = useMemo(
    () => averageVisits(frame?.response.combos, frame?.response.counts),
    [frame],
  );

  const board = pending ? pending.board : (node?.board ?? '');
  const slots = pending ? pending.needed : 0;

  return (
    <aside
      className="flex w-[300px] shrink-0 flex-col border-r"
      style={{ borderColor: 'var(--color-rule)', background: 'var(--color-slate)' }}
    >
      <div className="border-b px-4 py-3" style={{ borderColor: 'var(--color-rule)' }}>
        <Spec label="pot">{transition ? transition.pot.toLocaleString() : '—'}</Spec>
        <Spec label="board">
          {board || slots > 0 ? (
            <CardRow board={board} slots={slots} />
          ) : (
            <span style={{ color: 'var(--color-ash)' }}>preflop</span>
          )}
        </Spec>
        <Spec label="players">{node ? node.heads : '—'}</Spec>
        <Spec label="visits">{average === null ? '—' : formatVisits(average)}</Spec>
        <div className="flex items-center justify-between gap-3 pt-2">
          <span
            className="text-[10px] tracking-[0.14em] uppercase"
            style={{ color: 'var(--color-ash)' }}
          >
            temp
          </span>
          <span className="flex items-center gap-2">
            <input
              type="range"
              min={TEMP_MIN}
              max={TEMP_MAX}
              step={0.01}
              value={Math.min(TEMP_MAX, invTemp)}
              onChange={(event) => state.setInvTemp(Number(event.target.value))}
              className="h-1 w-[110px] cursor-pointer appearance-none rounded-full"
              style={{ background: 'var(--color-rule)', accentColor: 'var(--color-chalk)' }}
              aria-label="inverse temperature"
            />
            <span className="w-10 text-right text-[12px]">{invTemp.toFixed(2)}</span>
          </span>
        </div>
      </div>

      <div className="min-h-0 flex-1 overflow-y-auto px-4 py-2">
        {ledger.map((line, i) => {
          if (line.kind === 'street') {
            return (
              <div key={i} className="flex items-center gap-2 py-2">
                <span
                  className="text-[10px] tracking-[0.16em] uppercase"
                  style={{ color: 'var(--color-ash)' }}
                >
                  {line.street}
                </span>
                <span className="h-px flex-1" style={{ background: 'var(--color-rule)' }} />
              </div>
            );
          }

          if (line.kind === 'post') {
            return (
              <div
                key={i}
                className="flex items-baseline justify-between py-[2px] text-[12px]"
                style={{ color: 'var(--color-ash)' }}
              >
                <span className="w-14 shrink-0">{seatNames[line.seat ?? 0]}</span>
                <span className="flex-1">{line.name}</span>
                <span>{line.stack}</span>
              </div>
            );
          }

          if (line.kind === 'terminal') {
            return (
              <div key={i} className="py-2">
                <div className="flex items-baseline justify-between text-[12px]">
                  <span className="w-14 shrink-0">{seatNames[line.seat ?? 0]}</span>
                  <span className="flex-1" style={{ color: OPTION_COLORS[line.option ?? 0] }}>
                    {OPTION_NAMES[line.option ?? 0]}
                  </span>
                </div>
                <div
                  className="mt-2 text-[10px] tracking-[0.16em] uppercase"
                  style={{ color: 'var(--color-ash)' }}
                >
                  hand complete
                </div>
              </div>
            );
          }

          const option = line.option ?? 0;
          const raised = option >= 2;
          return (
            <div key={i} className="flex items-baseline justify-between py-[2px] text-[12px]">
              <span className="w-14 shrink-0">{seatNames[line.seat ?? 0]}</span>
              <span className="flex-1" style={{ color: OPTION_COLORS[option] }}>
                {OPTION_NAMES[option]}
                {raised && line.delta !== undefined ? (
                  <span className="ml-1.5" style={{ color: 'var(--color-chalk)' }}>
                    +{formatChips(line.delta)}
                  </span>
                ) : null}
              </span>
              <span style={{ color: 'var(--color-ash)' }}>{formatChips(line.stack ?? 0)}</span>
            </div>
          );
        })}
      </div>

      <div className="border-t px-4 py-3" style={{ borderColor: 'var(--color-rule)' }}>
        {mode === 'boardEntry' && pending ? (
          <>
            <div className="mb-2 text-[10px] tracking-[0.16em] uppercase" style={{ color: 'var(--color-ash)' }}>
              {pending.street} — {pending.rank ? 'suit' : 'rank'}
            </div>
            <div className="flex flex-wrap gap-1">
              {(pending.rank ? ['s', 'h', 'd', 'c'] : ['a', 'k', 'q', 'j', 't', '9…2']).map((key) => (
                <Keycap key={key} label={key} />
              ))}
            </div>
          </>
        ) : mode === 'ended' ? (
          <>
            <div className="mb-2 text-[10px] tracking-[0.16em] uppercase" style={{ color: 'var(--color-ash)' }}>
              deal again
            </div>
            <div className="flex gap-1">
              <Keycap label="n" tint="var(--color-chalk)" />
              <Keycap label="⌫" />
            </div>
          </>
        ) : (
          <>
            <div className="mb-2 flex items-baseline justify-between">
              <span className="text-[12px]">{node ? seatNames[node.seat] : '—'}</span>
              <span className="text-[12px]" style={{ color: 'var(--color-ash)' }}>
                {frame ? formatChips(frame.stacks[node?.seat ?? 0]) : ''}
              </span>
            </div>
            <div className="flex flex-wrap gap-1">
              {node?.legal.map((legal, option) =>
                legal ? (
                  <Keycap
                    key={option}
                    label={OPTION_KEYS[option]}
                    tint={OPTION_COLORS[option]}
                  />
                ) : null,
              )}
            </div>
          </>
        )}
        {error ? (
          <div className="mt-2 text-[11px]" style={{ color: 'var(--color-act-big)' }}>
            {error}
          </div>
        ) : null}
        {loading ? (
          <div className="mt-2 text-[10px] tracking-[0.16em] uppercase" style={{ color: 'var(--color-ash)' }}>
            reading
          </div>
        ) : null}
      </div>
    </aside>
  );
}

function formatChips(value: number): string {
  return Number.isInteger(value) ? String(value) : value.toFixed(1);
}

/** Visits run from single digits on a deep river node to millions preflop. */
function formatVisits(value: number): string {
  if (value >= 1e6) return `${(value / 1e6).toFixed(2)}M`;
  if (value >= 1e3) return `${(value / 1e3).toFixed(1)}k`;
  return value.toFixed(1);
}
