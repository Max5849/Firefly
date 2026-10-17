import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { ApiError, getMeta, postStrategy } from '../api/client';
import type { ApiFailure } from '../api/client';
import type { Config, MetaResponse, StrategyResponse } from '../api/types';
import { metaStats } from '../grid/metas';
import { cardCount, optionFromKey, rankFromKey, suitFromKey } from './keys';

/** One settled node: the action list that reached it and the answer it gave. */
interface Frame {
  actions: number[];
  board: string;
  response: StrategyResponse;
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
  /** What the table would call it: small blind, check, call, bet, raise, all in. */
  name?: string;
  option?: number;
  stack?: number;
  /** Total in front after the action - a raise "to" this figure. */
  size?: number;
  street?: string;
  board?: string;
  /** terminal: "fold" when one seat is left, "showdown" when the board runs out. */
  endedBy?: string;
  /** Frame this line belongs to, so clicking it can return to that state. */
  frame?: number;
}

export type MissingStrategy = Extract<ApiFailure, { kind: 'strategyMissing' }>;

export interface HandState {
  meta: MetaResponse | null;
  config: Config | null;
  mode: Mode;
  frame: Frame | null;
  pending: Pending | null;
  ledger: LedgerLine[];
  error: string | null;
  /** Set when the chosen config has no strategy file. */
  missing: MissingStrategy | null;
  loading: boolean;
  /** Seat names at this table size. */
  seatNames: string[];
  /** Total bet each option would produce, indexed by option. */
  sizes: (number | null)[];
  /** Combinations whose cluster is too thin to read at this node. */
  black: Set<number>;
  /** Visits summed over the distinct records the acting range reaches. */
  totalVisits: number | null;
  /**
   * The inverse temperature, per chip of EV, is ln(2^(2^(tempStep / 8))): each step
   * multiplies it by 2^(1/8). The page asks for every node at it.
   */
  tempStep: number;
  /** The steps a request may use, inclusive: ln(2^(1/64)) .. ln(2^4). */
  tempLimits: [number, number];
}

/** In eighths of a power of two: 2^(1/64) = 2^(2^-6) .. 2^4 = 2^(2^2). */
const TEMP_LIMITS: [number, number] = [-48, 16];

/** ln(2^(2^(step / 8))), the inverse temperature a step stands for. */
export function invTempOf(step: number): number {
  return Math.pow(2, step / 8) * Math.LN2;
}

export function useHand(): HandState & {
  handleKey: (key: string) => void;
  jumpTo: (frame: number) => void;
  setConfig: (config: Config) => void;
  setTempStep: (step: number) => void;
} {
  const [meta, setMeta] = useState<MetaResponse | null>(null);
  const [config, setConfigState] = useState<Config | null>(null);
  const [frames, setFrames] = useState<Frame[]>([]);
  const [pending, setPending] = useState<Pending | null>(null);
  const [mode, setMode] = useState<Mode>('acting');
  const [terminal, setTerminal] = useState<LedgerLine | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [missing, setMissing] = useState<MissingStrategy | null>(null);
  const [loading, setLoading] = useState(false);
  const [tempStep, setTempStepState] = useState(0);
  // Read by send(), so every request goes out at the temperature current when it is
  // sent without each caller having to carry it.
  const invTemp = useRef(invTempOf(0));

  // A newer keystroke must win, so every request supersedes the one before it.
  const inflight = useRef<AbortController | null>(null);

  const send = useCallback(
    async (
      setup: Config,
      actions: number[],
      board: string,
    ): Promise<{ ok: true; frame: Frame } | { ok: false; failure: ApiFailure }> => {
      inflight.current?.abort();
      const controller = new AbortController();
      inflight.current = controller;
      setLoading(true);
      try {
        const response = await postStrategy(
          { config: setup, invTemp: invTemp.current, actions, board },
          controller.signal,
        );
        return { ok: true, frame: { actions, board, response } };
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

  /** Back to the opening preflop node, at this config. */
  const reset = useCallback(
    async (setup: Config) => {
      // Cleared before the request, not after it: leaving the old frame in place
      // would show the previous position until the new one arrives.
      inflight.current?.abort();
      setFrames([]);
      setPending(null);
      setTerminal(null);
      setError(null);
      setMissing(null);
      setMode('acting');
      const result = await send(setup, [], '');
      if (result.ok) {
        setFrames([result.frame]);
      } else if (result.failure.kind === 'strategyMissing') {
        setMissing(result.failure);
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
        // A server from before the game / key / spot contract has no defaults, and
        // every request this page sends would be rejected by it.
        if (!loaded.defaults) {
          setError(
            'the API is an older server build than this page - rebuild and restart build/server from the current source',
          );
          return;
        }
        setMeta(loaded);
        // Start at the server's default, on the nearest step.
        const step = Math.min(
          TEMP_LIMITS[1],
          Math.max(TEMP_LIMITS[0], Math.round(8 * Math.log2(loaded.invTemp / Math.LN2))),
        );
        setTempStepState(step);
        invTemp.current = invTempOf(step);
        const { spec: _spec, ...defaults } = loaded.defaults;
        setConfigState(defaults);
        void reset(defaults);
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

  const setConfig = useCallback(
    (next: Config) => {
      setConfigState(next);
      void reset(next);
    },
    [reset],
  );

  const setTempStep = useCallback((step: number) => {
    const clamped = Math.min(TEMP_LIMITS[1], Math.max(TEMP_LIMITS[0], Math.round(step)));
    invTemp.current = invTempOf(clamped);
    setTempStepState(clamped);
  }, []);

  const top = frames.length > 0 ? frames[frames.length - 1] : null;

  // A frame answered at another temperature - the temperature just changed, or undo
  // or a ledger jump uncovered an older frame - is asked for again, so ranges, mixes
  // and EVs on screen always belong to the temperature shown.
  useEffect(() => {
    const served = top?.response.spot?.invTemp;
    if (!top || !config || served === undefined) return;
    if (Math.abs(served - invTempOf(tempStep)) < 1e-9) return;
    void send(config, top.actions, top.board).then((result) => {
      if (!result.ok) {
        if (result.failure.message !== 'superseded') setError(result.failure.message);
        return;
      }
      setFrames((current) => {
        const last = current[current.length - 1];
        return last === top ? [...current.slice(0, -1), result.frame] : current;
      });
    });
  }, [top, config, tempStep, send]);

  const settle = useCallback((frame: Frame) => {
    setFrames((current) => [...current, frame]);
    setError(null);
  }, []);

  const play = useCallback(
    async (option: number) => {
      if (!top || !config) return;
      const actions = [...top.actions, option];
      const result = await send(config, actions, top.board);
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
            seat: top.response.game?.actor.seat,
            name: top.response.game?.options[option]?.name,
            option,
            endedBy: failure.endedBy,
          });
          setMode('ended');
          setError(null);
          break;
        case 'illegal':
          setError(failure.reason);
          break;
        case 'strategyMissing':
          setMissing(failure);
          break;
        case 'other':
          if (failure.message !== 'superseded') setError(failure.message);
          break;
      }
    },
    [top, config, send, settle],
  );

  const commitBoard = useCallback(
    async (held: Pending) => {
      if (!top || !config) return;
      const actions = [...top.actions, held.action];
      const result = await send(config, actions, held.board);
      if (result.ok) {
        setPending(null);
        setMode('acting');
        settle(result.frame);
      } else if (result.failure.kind === 'handEnded') {
        setPending(null);
        setTerminal({
          kind: 'terminal',
          seat: top.response.game?.actor.seat,
          name: top.response.game?.options[held.action]?.name,
          option: held.action,
          endedBy: result.failure.endedBy,
        });
        setMode('ended');
      } else if (result.failure.message !== 'superseded') {
        // A duplicate card lands here. Drop the last card so the board can be
        // retyped rather than stranding the hand.
        setPending({ ...held, board: held.board.slice(0, -2), rank: null });
        setError(result.failure.message);
      }
    },
    [top, config, send, settle],
  );

  const undo = useCallback(() => {
    setError(null);
    if (mode === 'ended') {
      // The ending action never pushed a frame, so there is nothing to pop.
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
      if (key === '[' || key === ']') {
        setTempStep(tempStep + (key === ']' ? 1 : -1));
        return;
      }
      if (key === 'n' && mode !== 'boardEntry') {
        if (config) void reset(config);
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
      if (option >= 0 && top.response.game?.legal[option]) void play(option);
    },
    [mode, pending, top, config, reset, undo, play, commitBoard, tempStep, setTempStep],
  );

  const stats = useMemo(() => metaStats(top?.response.spot), [top]);
  const players = top?.response.game?.config.players ?? config?.players ?? 0;
  const seatNames = meta?.seats[String(players)] ?? [];
  const live = mode !== 'ended' ? top : null;

  return {
    meta,
    config,
    mode,
    frame: top,
    pending,
    ledger: buildLedger(frames, terminal),
    error,
    missing,
    loading,
    seatNames,
    sizes: live?.response.game?.options.map((option) => (option.legal ? option.betTo : null)) ?? [
      null,
      null,
      null,
      null,
      null,
    ],
    black: stats?.black ?? new Set<number>(),
    totalVisits: live?.response.spot?.totalVisits ?? null,
    tempStep,
    tempLimits: TEMP_LIMITS,
    setTempStep,
    handleKey,
    jumpTo,
    setConfig,
  };
}

/**
 * Read from the server's history at the newest node, so undo and jump-to cannot
 * leave a stale line behind. The k-th action produced frame k + 1.
 */
function buildLedger(frames: Frame[], terminal: LedgerLine | null): LedgerLine[] {
  const top = frames.length > 0 ? frames[frames.length - 1] : null;
  const history = top?.response.game?.history;
  if (!history) return [];
  const lines: LedgerLine[] = [];
  let actions = 0;
  for (const entry of history) {
    if (entry.kind === 'street') {
      lines.push({ kind: 'street', street: entry.street, board: entry.board });
      continue;
    }
    if (entry.kind === 'action') actions++;
    lines.push({
      kind: entry.kind,
      seat: entry.seat,
      name: entry.name,
      option: entry.option,
      stack: entry.stack,
      size: entry.betTo,
      frame: entry.kind === 'post' ? 0 : actions,
    });
  }
  if (terminal) lines.push(terminal);
  return lines;
}
