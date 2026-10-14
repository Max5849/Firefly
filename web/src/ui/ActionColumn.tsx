import type { HandState, LedgerLine } from '../hand/useHand';
import { OPTION_COLORS, OPTION_KEYS } from '../hand/seats';
import { CardRow } from './Card';
import { Keycap } from './Keycap';
import { formatChips, formatVisits } from './format';

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
  jumpTo: (frame: number) => void;
};

/** Every seat as the table shows it: stack behind, chips in front, and whether it is still in. */
function Seats({ state }: { state: Props }) {
  const game = state.mode === 'ended' ? null : state.frame?.response.game;
  if (!game) return null;
  return (
    <div className="border-b px-4 py-2" style={{ borderColor: 'var(--color-rule)' }}>
      <div className="mb-1 grid grid-cols-[2.5rem_1fr_1fr_3.5rem] gap-x-2 text-[10px] tracking-[0.14em] uppercase" style={{ color: 'var(--color-ash)' }}>
        <span>seat</span>
        <span className="text-right">stack</span>
        <span className="text-right">bet</span>
        <span className="text-right">state</span>
      </div>
      {game.seats.map((seat) => {
        const status = seat.acting ? 'to act' : !seat.inHand ? 'folded' : seat.allIn ? 'all in' : '';
        return (
          <div
            key={seat.seat}
            className="grid grid-cols-[2.5rem_1fr_1fr_3.5rem] gap-x-2 py-[2px] text-[12px]"
            style={{
              color: seat.inHand ? 'var(--color-chalk)' : 'var(--color-ash)',
              fontWeight: seat.acting ? 600 : 400,
            }}
          >
            <span>{seat.name}</span>
            <span className="text-right">{formatChips(seat.stack)}</span>
            <span className="text-right">{seat.bet > 0 ? formatChips(seat.bet) : '—'}</span>
            <span className="text-right text-[10px]" style={{ color: seat.acting ? 'var(--color-chalk)' : 'var(--color-ash)' }}>
              {status}
            </span>
          </div>
        );
      })}
    </div>
  );
}

/** The decision at the current node, rendered inline under the last action. */
function ActingBlock({ state }: { state: Props }) {
  const { frame, pending, mode, sizes } = state;
  const game = frame?.response.game;

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

  if (mode === 'ended' || !game) return null;

  return (
    <div className="mt-1 border-l-2 pl-2" style={{ borderColor: 'var(--color-chalk)' }}>
      <div className="mb-1.5 flex items-baseline justify-between text-[12px]">
        <span>{game.actor.name}</span>
        <span style={{ color: 'var(--color-ash)' }}>
          {game.actor.toCall > 0 ? `${formatChips(game.actor.toCall)} to call` : 'no bet'}
        </span>
      </div>
      <div className="flex flex-col gap-1">
        {game.options.map((option) => {
          if (!option.legal) return null;
          const size = sizes[option.option];
          const sized = option.option >= 1 && size !== null && size > 0;
          return (
            <div key={option.option} className="flex items-center gap-2">
              <Keycap label={OPTION_KEYS[option.option]} tint={OPTION_COLORS[option.option]} />
              <span className="text-[12px]" style={{ color: OPTION_COLORS[option.option] }}>
                {option.name}
                {sized && option.option !== 1 ? ` to ${formatChips(size)}` : ''}
                {option.option === 1 && option.adds ? ` ${formatChips(option.adds)}` : ''}
              </span>
              {option.size ? (
                <span className="text-[10px]" style={{ color: 'var(--color-ash)' }}>
                  {option.size}
                </span>
              ) : null}
            </div>
          );
        })}
      </div>
    </div>
  );
}

export function ActionColumn(state: Props) {
  const { frame, pending, mode, ledger, seatNames, error, loading, totalVisits } = state;
  const game = mode === 'ended' ? null : frame?.response.game;

  const board = pending ? pending.board : (frame?.response.game?.board ?? '');
  const slots = pending ? pending.needed : 0;

  // The acting block sits directly under the newest line - the last action, or the
  // street it opened - so it is rendered as part of the ledger flow rather than
  // pinned to the foot of the column.
  const lastActionIndex = ledger.reduce((found, line, i) => (line.kind !== 'terminal' ? i : found), -1);

  return (
    <aside
      className="flex w-[300px] shrink-0 flex-col border-r"
      style={{ borderColor: 'var(--color-rule)', background: 'var(--color-slate)' }}
    >
      <div className="border-b px-4 py-3" style={{ borderColor: 'var(--color-rule)' }}>
        <Spec label="street">{pending ? pending.street : (game?.street ?? '—')}</Spec>
        <Spec label="pot">{game ? formatChips(game.pot) : '—'}</Spec>
        <Spec label="board">
          {board || slots > 0 ? (
            <CardRow board={board} slots={slots} />
          ) : (
            <span style={{ color: 'var(--color-ash)' }}>preflop</span>
          )}
        </Spec>
        <Spec label="players">{game ? `${game.heads} of ${game.seats.length}` : '—'}</Spec>
        <Spec label="visits">{totalVisits !== null ? formatVisits(totalVisits) : '—'}</Spec>
      </div>

      <Seats state={state} />

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
    </aside>
  );
}

function renderLine(line: LedgerLine, seatNames: string[], jumpTo: (frame: number) => void) {
  if (line.kind === 'street') {
    return (
      <div className="flex items-center gap-2 py-2">
        <span className="text-[10px] tracking-[0.16em] uppercase" style={{ color: 'var(--color-ash)' }}>
          {line.street}
        </span>
        <span className="text-[10px]" style={{ color: 'var(--color-ash)' }}>
          {line.board}
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
        <span className="flex-1">
          {line.name} {formatChips(line.size ?? 0)}
        </span>
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
            {line.name}
          </span>
        </div>
        <div className="mt-2 text-[10px] tracking-[0.16em] uppercase" style={{ color: 'var(--color-ash)' }}>
          {line.endedBy === 'showdown' ? 'hand complete · to showdown' : 'hand complete'}
        </div>
        <div className="mt-1.5 text-[10px]" style={{ color: 'var(--color-ash)' }}>
          n for a new hand
        </div>
      </div>
    );
  }

  const option = line.option ?? 0;
  const raised = option >= 2 && line.size !== undefined;
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
        {line.name}
        {raised ? ` to ${formatChips(line.size!)}` : ''}
      </span>
      <span style={{ color: 'var(--color-ash)' }}>{formatChips(line.stack ?? 0)}</span>
    </button>
  );
}
