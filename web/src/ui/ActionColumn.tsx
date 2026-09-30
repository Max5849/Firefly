import { useMemo } from 'react';
import type { HandState } from '../hand/useHand';
import { OPTION_COLORS, OPTION_KEYS, OPTION_NAMES, TEMPS } from '../hand/seats';
import { metaStats } from '../grid/metas';
import { COMBO_CARDS, comboName } from '../grid/comboIndex';
import { CardRow } from './Card';
import { Keycap } from './Keycap';

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

type Props = HandState & {
  setInvTemp: (value: number) => void;
  jumpTo: (frame: number) => void;
  /** Combination under the cursor in the grid, or null. */
  hovered: number | null;
};

/** What the grid is showing for one combination, read off on hover. */
function KeyReadout({ state }: { state: Props }) {
  const { hovered, frame, reach, black } = state;
  if (hovered === null || !frame) return null;
  const row = frame.response.combos?.[hovered];
  const [first, second] = COMBO_CARDS[hovered];
  const name = comboName(first, second);

  const lines: [string, string][] = [];
  if (!row) {
    lines.push(['status', 'uses a board card']);
  } else if (!row.found) {
    lines.push(['status', row.metaFound === false ? 'no hand meta' : 'not in the table']);
  } else {
    lines.push(['key', row.key ?? '-']);
    lines.push(['meta', String(row.handMeta ?? '-')]);
    if (typeof row.nodeVisits === 'number' && typeof row.metaCombos === 'number') {
      lines.push(['visits', `${row.nodeVisits.toLocaleString()} / ${row.metaCombos}`]);
    }
    const probability = reach ? reach[hovered] : 1;
    lines.push(['reach', probability.toFixed(4)]);
    if (black.has(hovered)) lines.push(['status', 'below 1% - black']);
  }

  return (
    <div className="shrink-0 border-t px-4 py-2" style={{ borderColor: 'var(--color-rule)' }}>
      <div className="mb-1 flex items-baseline justify-between">
        <span className="text-[12px]">{name}</span>
        <span className="text-[10px] tracking-[0.14em] uppercase" style={{ color: 'var(--color-ash)' }}>
          hovered
        </span>
      </div>
      {lines.map(([label, value]) => (
        <div key={label} className="flex items-baseline justify-between gap-2 text-[11px]">
          <span style={{ color: 'var(--color-ash)' }}>{label}</span>
          <span className="truncate">{value}</span>
        </div>
      ))}
    </div>
  );
}

/** The decision at the current node, rendered inline under the last action. */
function ActingBlock({ state }: { state: Props }) {
  const { frame, pending, mode, seatNames, sizes } = state;
  const node = frame?.response.node;

  if (mode === 'boardEntry' && pending) {
    return (
      <div className="mt-1 border-l-2 pl-2" style={{ borderColor: 'var(--color-chalk)' }}>
        <div className="mb-1.5 text-[10px] tracking-[0.16em] uppercase" style={{ color: 'var(--color-ash)' }}>
          {pending.street} — {pending.rank ? `${pending.rank} · suit` : 'rank'}
        </div>
        <div className="flex flex-wrap gap-1">
          {(pending.rank ? ['s', 'h', 'd', 'c'] : ['a', 'k', 'q', 'j', 't', '9…2']).map((key) => (
            <Keycap key={key} label={key} />
          ))}
        </div>
      </div>
    );
  }

  if (mode === 'ended' || !node) return null;

  return (
    <div className="mt-1 border-l-2 pl-2" style={{ borderColor: 'var(--color-chalk)' }}>
      <div className="mb-1.5 flex items-baseline justify-between text-[12px]">
        <span>{seatNames[node.seat]}</span>
        <span style={{ color: 'var(--color-ash)' }}>{formatChips(frame.stacks[node.seat])}</span>
      </div>
      <div className="flex flex-col gap-1">
        {node.legal.map((legal, option) => {
          if (!legal) return null;
          const size = sizes[option];
          return (
            <div key={option} className="flex items-center gap-2">
              <Keycap label={OPTION_KEYS[option]} tint={OPTION_COLORS[option]} />
              <span className="text-[12px]" style={{ color: OPTION_COLORS[option] }}>
                {option >= 2 && size !== null ? formatChips(size) : OPTION_NAMES[option]}
              </span>
            </div>
          );
        })}
      </div>
    </div>
  );
}

export function ActionColumn(state: Props) {
  const { frame, pending, mode, ledger, seatNames, invTemp, error, loading } = state;
  const node = frame?.response.node;
  const transition = frame?.response.transition;

  const stats = useMemo(() => metaStats(frame?.response), [frame]);

  const board = pending ? pending.board : (node?.board ?? '');
  const slots = pending ? pending.needed : 0;

  // The acting block sits directly under the last action, so it is rendered as
  // part of the ledger flow rather than pinned to the foot of the column.
  const lastActionIndex = ledger.reduce(
    (found, line, i) => (line.kind === 'action' || line.kind === 'post' ? i : found),
    -1,
  );

  return (
    <aside
      className="flex w-[300px] shrink-0 flex-col border-r"
      style={{ borderColor: 'var(--color-rule)', background: 'var(--color-slate)' }}
    >
      <div className="border-b px-4 py-3" style={{ borderColor: 'var(--color-rule)' }}>
        <Spec label="pot">{transition ? formatChips(transition.pot) : '—'}</Spec>
        <Spec label="board">
          {board || slots > 0 ? (
            <CardRow board={board} slots={slots} />
          ) : (
            <span style={{ color: 'var(--color-ash)' }}>preflop</span>
          )}
        </Spec>
        <Spec label="players">{node ? node.heads : '—'}</Spec>
        <Spec label="visits">{stats && mode !== 'ended' ? formatVisits(stats.total) : '—'}</Spec>
        <div className="flex items-center justify-between gap-3 pt-2">
          <span className="text-[10px] tracking-[0.14em] uppercase" style={{ color: 'var(--color-ash)' }}>
            temp
          </span>
          <span className="flex gap-1">
            {TEMPS.map((temp) => {
              const active = Math.abs(invTemp - temp.value) < 1e-9;
              return (
                <button
                  key={temp.label}
                  type="button"
                  // The page is driven from the keyboard, so a control must never
                  // keep focus after a click - otherwise the next keystroke goes
                  // to the button instead of the hand. preventDefault stops the
                  // click taking focus; blur also releases it when the button was
                  // already focused, by Tab or by a previous activation.
                  onMouseDown={(event) => event.preventDefault()}
                  onClick={(event) => {
                    event.currentTarget.blur();
                    state.setInvTemp(temp.value);
                  }}
                  className="rounded-[3px] border px-2 py-[3px] text-[10px] tracking-[0.1em] uppercase"
                  style={{
                    borderColor: active ? 'var(--color-chalk)' : 'var(--color-rule)',
                    color: active ? 'var(--color-chalk)' : 'var(--color-ash)',
                  }}
                >
                  {temp.label}
                </button>
              );
            })}
          </span>
        </div>
      </div>

      <div className="min-h-0 flex-1 overflow-y-auto px-4 py-2">
        {ledger.map((line, i) => (
          <div key={i}>
            {renderLine(line, seatNames, state.jumpTo)}
            {i === lastActionIndex ? <ActingBlock state={state} /> : null}
          </div>
        ))}
        {ledger.length === 0 ? (
          <div className="text-[11px]" style={{ color: 'var(--color-ash)' }}>
            {loading ? 'reading' : ''}
          </div>
        ) : null}
        {error ? (
          <div className="mt-2 text-[11px]" style={{ color: 'var(--color-act-big)' }}>
            {error}
          </div>
        ) : null}
      </div>

      <KeyReadout state={state} />
    </aside>
  );
}

function renderLine(
  line: import('../hand/useHand').LedgerLine,
  seatNames: string[],
  jumpTo: (frame: number) => void,
) {
  if (line.kind === 'street') {
    return (
      <div className="flex items-center gap-2 py-2">
        <span className="text-[10px] tracking-[0.16em] uppercase" style={{ color: 'var(--color-ash)' }}>
          {line.street}
        </span>
        <span className="h-px flex-1" style={{ background: 'var(--color-rule)' }} />
      </div>
    );
  }

  if (line.kind === 'post') {
    return (
      <div
        className="flex items-baseline justify-between py-[2px] text-[12px]"
        style={{ color: 'var(--color-ash)' }}
      >
        <span className="w-10 shrink-0">{seatNames[line.seat ?? 0]}</span>
        <span className="flex-1">{line.name}</span>
        <span>{formatChips(line.stack ?? 0)}</span>
      </div>
    );
  }

  if (line.kind === 'terminal') {
    return (
      <div className="py-2">
        <div className="flex items-baseline justify-between text-[12px]">
          <span className="w-10 shrink-0">{seatNames[line.seat ?? 0]}</span>
          <span className="flex-1" style={{ color: OPTION_COLORS[line.option ?? 0] }}>
            {OPTION_NAMES[line.option ?? 0]}
          </span>
        </div>
        <div className="mt-2 text-[10px] tracking-[0.16em] uppercase" style={{ color: 'var(--color-ash)' }}>
          hand complete
        </div>
        <div className="mt-1.5 text-[10px]" style={{ color: 'var(--color-ash)' }}>
          n for a new hand
        </div>
      </div>
    );
  }

  const option = line.option ?? 0;
  return (
    <button
      type="button"
      onMouseDown={(event) => event.preventDefault()}
      onClick={(event) => {
        event.currentTarget.blur();
        if (line.frame !== undefined) jumpTo(line.frame);
      }}
      title="return to this point"
      className="flex w-full cursor-pointer items-baseline justify-between py-[2px] text-left text-[12px] hover:bg-white/5"
    >
      <span className="w-10 shrink-0">{seatNames[line.seat ?? 0]}</span>
      <span className="flex-1" style={{ color: OPTION_COLORS[option] }}>
        {option >= 2 && line.size !== undefined ? formatChips(line.size) : OPTION_NAMES[option]}
      </span>
      <span style={{ color: 'var(--color-ash)' }}>{formatChips(line.stack ?? 0)}</span>
    </button>
  );
}

function formatChips(value: number): string {
  return Number.isInteger(value) ? String(value) : value.toFixed(1);
}

/** Node totals run from the hundreds on a deep river to billions preflop. */
function formatVisits(value: number): string {
  if (value >= 1e9) return `${(value / 1e9).toFixed(2)}B`;
  if (value >= 1e6) return `${(value / 1e6).toFixed(2)}M`;
  if (value >= 1e3) return `${(value / 1e3).toFixed(1)}k`;
  return String(Math.round(value));
}
