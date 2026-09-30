import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { ApiError, FULL_INCLUDE, getMeta, postStrategy } from '../api/client';
import type { MetaResponse, StrategyResponse } from '../api/types';
import { belowCut, reachProbabilities } from '../grid/range';
import { LN65536, SEAT_NAMES } from './seats';
import { cardCount, optionFromKey, rankFromKey, suitFromKey } from './keys';

/** One settled node: the action list that reached it and the answer it gave. */
interface Frame {
  actions: number[];
  board: string;
  response: StrategyResponse;
  /** Every seat's stack at this node, carried forward from lastAction.chips. */
  stacks: number[];
  /** Every seat's chips in front on the current street, zeroed when it rolls. */
  bets: number[];
}

/** The action the player has committed to but cannot send until the board is in. */
interface Pending {
  action: number;
  board: string;
  rank: string | null;
  needed: number;
  street: string;
}

export type Mode = 'acting' | 'boardEntry' | 'ended';

export interface LedgerLine {
  kind: 'post' | 'action' | 'street' | 'terminal';
  seat?: number;
  name?: string;
  option?: number;
  stack?: number;
  /** Total chips in front after the action - a raise "to" this figure. */
  size?: number;
  street?: string;
  board?: string;
  /** Frame this line belongs to, so clicking it can return to that state. */
  frame?: number;
}

export interface HandState {
  meta: MetaResponse | null;
  mode: Mode;
  frame: Frame | null;
  pending: Pending | null;
  ledger: LedgerLine[];
  invTemp: number;
  error: string | null;
  loading: boolean;
  seatNames: string[];
  /** Total bet each option would produce, indexed by option. */
  sizes: (number | null)[];
  /**
   * The acting player's own range: combinations whose probability of having
   * reached this node, given their previous decisions, has fallen under 1%.
   * Other seats' actions never narrow it.
   */
  black: Set<number>;
  /** The acting seat's reach probabilities, or null before they have acted. */
  reach: Float64Array | null;
}

function seedStacks(meta: MetaResponse): number[] {
  const stacks = new Array<number>(meta.playerCount).fill(meta.startingChips);
  stacks[1] -= meta.blinds[0];
  stacks[2] -= meta.blinds[1];
  return stacks;
}

function seedBets(meta: MetaResponse): number[] {
  const bets = new Array<number>(meta.playerCount).fill(0);
  bets[1] = meta.blinds[0];
  bets[2] = meta.blinds[1];
  return bets;
}

/**
 * What each option would make the actor's total bet. play() first matches the
 * outstanding bet, then adds k times the pot *after* that call - so a raise ends
 * at (table bet) + k * potAfterCall, and an all-in at their whole stack.
 */
function raiseSizes(response: StrategyResponse | null): (number | null)[] {
  const actor = response?.transition?.actor;
  const legal = response?.node?.legal;
  if (!actor || !legal) return [null, null, null, null, null];
  const tableBet = actor.bet + actor.toCall;
  return [
    null,
    legal[1] ? tableBet : null,
    legal[2] ? tableBet + 0.5 * actor.potAfterCall : null,
    legal[3] ? tableBet + 1.5 * actor.potAfterCall : null,
    legal[4] ? actor.bet + actor.chips : null,
  ];
}

export function useHand(): HandState & {
  handleKey: (key: string) => void;
  setInvTemp: (value: number) => void;
  jumpTo: (frame: number) => void;
} {
  const [meta, setMeta] = useState<MetaResponse | null>(null);
  const [frames, setFrames] = useState<Frame[]>([]);
  const [pending, setPending] = useState<Pending | null>(null);
  const [mode, setMode] = useState<Mode>('acting');
  const [terminal, setTerminal] = useState<LedgerLine | null>(null);
  const [invTemp, setInvTempState] = useState<number>(LN65536);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  // A newer keystroke must win, so every request supersedes the one before it.
  const inflight = useRef<AbortController | null>(null);

  const send = useCallback(
    async (
      actions: number[],
      board: string,
      temp: number,
      previous: Frame | null,
      fallbackStacks?: number[],
      fallbackBets?: number[],
    ): Promise<{ ok: true; frame: Frame } | { ok: false; failure: ApiError['failure'] }> => {
      inflight.current?.abort();
      const controller = new AbortController();
      inflight.current = controller;
      setLoading(true);
      try {
        const response = await postStrategy(
          { actions, board, invTemp: temp, include: { ...FULL_INCLUDE } },
          controller.signal,
        );

        const beforeStacks = previous?.stacks ?? fallbackStacks ?? [];
        const beforeBets = previous?.bets ?? fallbackBets ?? [];
        const stacks = beforeStacks.slice();
        let bets = beforeBets.slice();

        const last = response.transition?.lastAction;
        if (last) {
          const spent = beforeStacks[last.seat] - last.chips;
          stacks[last.seat] = last.chips;
          const rolled = (response.node?.round ?? 0) > (previous?.response.node?.round ?? 0);
          if (rolled) {
            bets = new Array<number>(beforeBets.length).fill(0);
          } else {
            bets[last.seat] = (beforeBets[last.seat] ?? 0) + spent;
          }
        }
        return { ok: true, frame: { actions, board, response, stacks, bets } };
      } catch (caught) {
        if (caught instanceof ApiError) return { ok: false, failure: caught.failure };
        if (caught instanceof DOMException && caught.name === 'AbortError') {
          return { ok: false, failure: { kind: 'other', message: 'superseded' } };
        }
        return {
          ok: false,
          failure: { kind: 'other', message: caught instanceof Error ? caught.message : 'failed' },
        };
      } finally {
        if (inflight.current === controller) {
          inflight.current = null;
          setLoading(false);
        }
      }
    },
    [],
  );

  /** Back to the opening preflop node. */
  const reset = useCallback(
    async (loaded: MetaResponse, temp: number) => {
      // Cleared before the request, not after it: leaving the old frame in place
      // would show the previous position until the new one arrives, which reads
      // as the grid flashing the hand you just abandoned.
      inflight.current?.abort();
      setFrames([]);
      setPending(null);
      setTerminal(null);
      setError(null);
      setMode('acting');
      const result = await send([], '', temp, null, seedStacks(loaded), seedBets(loaded));
      if (result.ok) {
        setFrames([result.frame]);
      } else if (result.failure.message !== 'superseded') {
        setError(result.failure.message);
      }
    },
    [send],
  );

  useEffect(() => {
    let cancelled = false;
    getMeta()
      .then((loaded) => {
        if (cancelled) return;
        setMeta(loaded);
        void reset(loaded, LN65536);
      })
      .catch((caught: unknown) => {
        if (!cancelled) {
          setError(caught instanceof Error ? caught.message : 'could not reach the server');
        }
      });
    return () => {
      cancelled = true;
    };
  }, [reset]);

  const top = frames.length > 0 ? frames[frames.length - 1] : null;

  const settle = useCallback((frame: Frame) => {
    setFrames((current) => [...current, frame]);
    setError(null);
  }, []);

  const play = useCallback(
    async (option: number) => {
      if (!top) return;
      const actions = [...top.actions, option];
      const result = await send(actions, top.board, invTemp, top);
      if (result.ok) {
        settle(result.frame);
        return;
      }
      const { failure } = result;
      switch (failure.kind) {
        case 'needsBoard':
          setPending({
            action: option,
            board: top.board,
            rank: null,
            needed: failure.needed,
            street: failure.street,
          });
          setMode('boardEntry');
          setError(null);
          break;
        case 'handEnded':
          setTerminal({
            kind: 'terminal',
            seat: top.response.node?.seat,
            option,
          });
          setMode('ended');
          setError(null);
          break;
        case 'illegal':
          setError(failure.reason);
          break;
        case 'other':
          if (failure.message !== 'superseded') setError(failure.message);
          break;
      }
    },
    [top, invTemp, send, settle],
  );

  const commitBoard = useCallback(
    async (held: Pending) => {
      if (!top) return;
      const actions = [...top.actions, held.action];
      const result = await send(actions, held.board, invTemp, top);
      if (result.ok) {
        setPending(null);
        setMode('acting');
        settle(result.frame);
      } else if (result.failure.message !== 'superseded') {
        // A duplicate card or an impossible runout lands here. Drop the last card
        // so the board can be retyped rather than stranding the hand.
        setPending({ ...held, board: held.board.slice(0, -2), rank: null });
        setError(result.failure.message);
      }
    },
    [top, invTemp, send, settle],
  );

  const undo = useCallback(() => {
    setError(null);
    if (mode === 'ended') {
      // The hand only ends by folding out now, and that never pushed a frame, so
      // there is nothing to pop.
      setTerminal(null);
      setMode('acting');
      return;
    }
    if (mode === 'boardEntry' && pending) {
      if (pending.rank) {
        setPending({ ...pending, rank: null });
        return;
      }
      if (cardCount(pending.board) > cardCount(top?.board ?? '')) {
        setPending({ ...pending, board: pending.board.slice(0, -2) });
        return;
      }
      setPending(null);
      setMode('acting');
      return;
    }
    setFrames((current) => (current.length > 1 ? current.slice(0, -1) : current));
  }, [mode, pending, top]);

  /** Return to the state a ledger line describes, discarding everything after it. */
  const jumpTo = useCallback((frame: number) => {
    inflight.current?.abort();
    setError(null);
    setPending(null);
    setTerminal(null);
    setMode('acting');
    setFrames((current) => (frame < current.length - 1 ? current.slice(0, frame + 1) : current));
  }, []);

  const handleKey = useCallback(
    (raw: string) => {
      const key = raw.length === 1 ? raw.toLowerCase() : raw;

      if (key === 'Backspace') {
        undo();
        return;
      }
      if (key === 'n' && mode !== 'boardEntry') {
        if (meta) void reset(meta, invTemp);
        return;
      }
      if (mode === 'ended' || !top) return;

      if (mode === 'boardEntry' && pending) {
        if (!pending.rank) {
          const rank = rankFromKey(key);
          if (rank) setPending({ ...pending, rank });
          return;
        }
        const suit = suitFromKey(key);
        if (!suit) return;
        const board = pending.board + pending.rank + suit;
        const next = { ...pending, board, rank: null };
        setPending(next);
        if (cardCount(board) === next.needed) void commitBoard(next);
        return;
      }

      const option = optionFromKey(key);
      // Illegal options are inert - the keycap row already says which keys do
      // anything, so a rejection message would be noise.
      if (option >= 0 && top.response.node?.legal[option]) void play(option);
    },
    [mode, pending, top, meta, invTemp, reset, undo, play, commitBoard],
  );

  const setInvTemp = useCallback(
    (value: number) => {
      setInvTempState(value);
      if (!top) return;
      const previous = frames.length > 1 ? frames[frames.length - 2] : null;
      void send(top.actions, top.board, value, previous, top.stacks, top.bets).then((result) => {
        if (result.ok) setFrames((current) => [...current.slice(0, -1), result.frame]);
      });
    },
    [top, frames, send],
  );

  // Derived from the frame stack, so undo and jump-to shrink it automatically.
  const reach = useMemo(() => reachProbabilities(frames), [frames]);
  const actingSeat = top?.response.node?.seat;
  const black = useMemo(
    () => (actingSeat === undefined ? new Set<number>() : belowCut(reach.get(actingSeat))),
    [reach, actingSeat],
  );
  const actingReach = useMemo(
    () => (actingSeat === undefined ? null : (reach.get(actingSeat) ?? null)),
    [reach, actingSeat],
  );

  return {
    meta,
    mode,
    frame: top,
    pending,
    ledger: buildLedger(frames, meta, terminal),
    invTemp,
    error,
    loading,
    seatNames: SEAT_NAMES,
    sizes: raiseSizes(mode === 'ended' ? null : (top?.response ?? null)),
    black,
    reach: actingReach,
    handleKey,
    setInvTemp,
    jumpTo,
  };
}

/**
 * Derived from the frame stack rather than accumulated, so undo and jump-to
 * cannot leave a stale line behind.
 */
function buildLedger(
  frames: Frame[],
  meta: MetaResponse | null,
  terminal: LedgerLine | null,
): LedgerLine[] {
  if (frames.length === 0 || !meta) return [];
  const lines: LedgerLine[] = [
    { kind: 'post', seat: 1, name: 'small blind', size: meta.blinds[0], stack: frames[0].stacks[1], frame: 0 },
    { kind: 'post', seat: 2, name: 'big blind', size: meta.blinds[1], stack: frames[0].stacks[2], frame: 0 },
  ];

  for (let i = 1; i < frames.length; i++) {
    const previous = frames[i - 1];
    const frame = frames[i];
    const last = frame.response.transition?.lastAction;
    if (!last) continue;

    const spent = previous.stacks[last.seat] - last.chips;
    lines.push({
      kind: 'action',
      seat: last.seat,
      option: last.option,
      stack: last.chips,
      // The total in front after the action. Never lastAction.bet, which reads 0
      // whenever the action closed the street.
      size: (previous.bets[last.seat] ?? 0) + spent,
      frame: i,
    });

    const before = previous.response.node?.round ?? 0;
    const after = frame.response.node?.round ?? 0;
    if (after > before) {
      lines.push({
        kind: 'street',
        street: frame.response.node?.street,
        board: frame.response.node?.board,
      });
    }
  }

  if (terminal) lines.push(terminal);
  return lines;
}
