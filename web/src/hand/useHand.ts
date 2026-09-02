import { useCallback, useEffect, useRef, useState } from 'react';
import { ApiError, FULL_INCLUDE, getMeta, postStrategy } from '../api/client';
import type { MetaResponse, StrategyResponse } from '../api/types';
import { SEAT_NAMES } from './seats';
import { cardCount, optionFromKey, rankFromKey, suitFromKey } from './keys';

/** One settled node: the action list that reached it and the answer it gave. */
interface Frame {
  actions: number[];
  board: string;
  response: StrategyResponse;
  /** Every seat's stack at this node, carried forward from lastAction.chips. */
  stacks: number[];
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
  delta?: number;
  street?: string;
  board?: string;
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
}

function seedStacks(meta: MetaResponse): number[] {
  const stacks = new Array<number>(meta.playerCount).fill(meta.startingChips);
  // Seat 1 posts the small blind, seat 2 the big blind.
  stacks[1] -= meta.blinds[0];
  stacks[2] -= meta.blinds[1];
  return stacks;
}

export function useHand(): HandState & {
  handleKey: (key: string) => void;
  setInvTemp: (value: number) => void;
} {
  const [meta, setMeta] = useState<MetaResponse | null>(null);
  const [frames, setFrames] = useState<Frame[]>([]);
  const [pending, setPending] = useState<Pending | null>(null);
  const [mode, setMode] = useState<Mode>('acting');
  const [terminal, setTerminal] = useState<LedgerLine | null>(null);
  const [invTemp, setInvTempState] = useState<number>(11.090354888959125);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(false);

  // A newer keystroke must win, so every request supersedes the one before it.
  const inflight = useRef<AbortController | null>(null);

  const send = useCallback(
    async (
      actions: number[],
      board: string,
      temp: number,
      stacksBefore: number[],
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
        const stacks = stacksBefore.slice();
        const last = response.transition?.lastAction;
        if (last) {
          stacks[last.seat] = last.chips;
        }
        return { ok: true, frame: { actions, board, response, stacks } };
      } catch (caught) {
        if (caught instanceof ApiError) {
          return { ok: false, failure: caught.failure };
        }
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
      setPending(null);
      setTerminal(null);
      setError(null);
      setMode('acting');
      const result = await send([], '', temp, seedStacks(loaded));
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
        setInvTempState(loaded.defaultInvTemp);
        void reset(loaded, loaded.defaultInvTemp);
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

  /** Send one action from the current node, routing the three interesting failures. */
  const play = useCallback(
    async (option: number) => {
      if (!top) return;
      const actions = [...top.actions, option];
      const result = await send(actions, top.board, invTemp, top.stacks);
      if (result.ok) {
        setFrames((current) => [...current, result.frame]);
        setError(null);
        return;
      }
      const { failure } = result;
      switch (failure.kind) {
        case 'needsBoard':
          // The action was legal; the street simply rolled over. Hold it until the
          // board is entered, then send the same list again.
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
    [top, invTemp, send],
  );

  /** The board is complete, so replay the held action against it. */
  const commitBoard = useCallback(
    async (held: Pending) => {
      if (!top) return;
      const actions = [...top.actions, held.action];
      const result = await send(actions, held.board, invTemp, top.stacks);
      if (result.ok) {
        setFrames((current) => [...current, result.frame]);
        setPending(null);
        setMode('acting');
        setError(null);
      } else if (result.failure.message !== 'superseded') {
        // A duplicate card or an impossible runout lands here. Drop the last card
        // so the board can be retyped rather than stranding the hand.
        setPending({ ...held, board: held.board.slice(0, -2), rank: null });
        setError(result.failure.message);
      }
    },
    [top, invTemp, send],
  );

  const undo = useCallback(() => {
    setError(null);
    if (mode === 'ended') {
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
      // Back past the first card of the new street: give up the held action too.
      setPending(null);
      setMode('acting');
      return;
    }
    setFrames((current) => (current.length > 1 ? current.slice(0, -1) : current));
  }, [mode, pending, top]);

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
        if (cardCount(board) === next.needed) {
          void commitBoard(next);
          setPending(next);
        } else {
          setPending(next);
        }
        return;
      }

      const option = optionFromKey(key);
      // Illegal options are simply inert - the keycap row already says which keys
      // do anything, so a rejection message would be noise.
      if (option >= 0 && top.response.node?.legal[option]) {
        void play(option);
      }
    },
    [mode, pending, top, meta, invTemp, reset, undo, play, commitBoard],
  );

  const setInvTemp = useCallback(
    (value: number) => {
      setInvTempState(value);
      if (!top) return;
      void send(top.actions, top.board, value, top.stacks).then((result) => {
        if (result.ok) {
          setFrames((current) => [...current.slice(0, -1), result.frame]);
        }
      });
    },
    [top, send],
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
    seatNames: meta?.seats ?? SEAT_NAMES,
    handleKey,
    setInvTemp,
  };
}

/**
 * The ledger is derived, never accumulated - it is a pure function of the frame
 * stack, so undo cannot leave a stale line behind.
 */
function buildLedger(
  frames: Frame[],
  meta: MetaResponse | null,
  terminal: LedgerLine | null,
): LedgerLine[] {
  if (frames.length === 0 || !meta) return [];
  const lines: LedgerLine[] = [
    { kind: 'post', seat: 1, name: 'small blind', delta: meta.blinds[0], stack: frames[0].stacks[1] },
    { kind: 'post', seat: 2, name: 'big blind', delta: meta.blinds[1], stack: frames[0].stacks[2] },
  ];

  for (let i = 1; i < frames.length; i++) {
    const previous = frames[i - 1];
    const frame = frames[i];
    const last = frame.response.transition?.lastAction;
    if (!last) continue;

    lines.push({
      kind: 'action',
      seat: last.seat,
      option: last.option,
      stack: last.chips,
      // Never lastAction.bet: it reads 0 whenever the action closed the street.
      delta: previous.stacks[last.seat] - last.chips,
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
