import type { MetaResponse, StrategyRequest, StrategyResponse } from './types';

// Dev: same-origin /api, proxied to 127.0.0.1:8080 by Vite - no CORS, no token.
// Deployed: VITE_API_URL points at nginx and VITE_API_TOKEN carries the bearer.
const BASE = (import.meta.env.VITE_API_URL as string | undefined)?.replace(/\/$/, '') ?? '/api';
const TOKEN = import.meta.env.VITE_API_TOKEN as string | undefined;

function headers(): HeadersInit {
  const h: Record<string, string> = { 'Content-Type': 'application/json' };
  if (TOKEN) {
    h.Authorization = `Bearer ${TOKEN}`;
  }
  return h;
}

/**
 * The server validates the board count *after* the replay, so a street rollover
 * is only ever announced as a rejection. These are the outcomes worth acting on
 * rather than merely reporting.
 */
export type ApiFailure =
  | { kind: 'needsBoard'; street: string; needed: number; message: string }
  | { kind: 'handEnded'; actionIndex: number; message: string }
  | { kind: 'illegal'; actionIndex: number; reason: string; message: string }
  | { kind: 'other'; message: string };

export class ApiError extends Error {
  readonly failure: ApiFailure;
  constructor(failure: ApiFailure) {
    super(failure.message);
    this.name = 'ApiError';
    this.failure = failure;
  }
}

const STREET_CARDS: Record<string, number> = { flop: 3, turn: 4, river: 5 };

function classify(body: Record<string, unknown>): ApiFailure {
  const message = typeof body.error === 'string' ? body.error : 'the request failed';

  // "the action list reaches the flop, which needs 3 board cards, but 0 were given"
  const needs = /needs (\d+) board cards/.exec(message);
  const street = /reaches the (flop|turn|river)/.exec(message);
  if (needs || (street && message.includes('board cards'))) {
    const name = street ? street[1] : 'flop';
    // Prefer the number the server stated; fall back to the street it named, so a
    // reworded message still routes to board entry rather than to an error line.
    const needed = needs ? Number(needs[1]) : STREET_CARDS[name];
    return { kind: 'needsBoard', street: name, needed, message };
  }

  if (message.includes('the hand ended at action')) {
    return {
      kind: 'handEnded',
      actionIndex: typeof body.actionIndex === 'number' ? body.actionIndex : -1,
      message,
    };
  }

  if (Array.isArray(body.legal) && typeof body.actionIndex === 'number') {
    return {
      kind: 'illegal',
      actionIndex: body.actionIndex,
      reason: typeof body.reason === 'string' ? body.reason : message,
      message,
    };
  }

  return { kind: 'other', message };
}

export async function getMeta(signal?: AbortSignal): Promise<MetaResponse> {
  const response = await fetch(`${BASE}/meta`, { headers: headers(), signal });
  if (!response.ok) {
    throw new ApiError({ kind: 'other', message: `GET /meta returned ${response.status}` });
  }
  return (await response.json()) as MetaResponse;
}

export async function postStrategy(
  request: StrategyRequest,
  signal?: AbortSignal,
): Promise<StrategyResponse> {
  const response = await fetch(`${BASE}/strategy`, {
    method: 'POST',
    headers: headers(),
    body: JSON.stringify(request),
    signal,
  });

  const text = await response.text();
  let body: unknown;
  try {
    body = JSON.parse(text);
  } catch {
    throw new ApiError({
      kind: 'other',
      message: `the server returned ${response.status} with a non-JSON body`,
    });
  }

  if (!response.ok) {
    throw new ApiError(classify(body as Record<string, unknown>));
  }
  return body as StrategyResponse;
}

/** Everything the server can return. Diagnostic sections are fetched but unrendered. */
export const FULL_INCLUDE = {
  node: true,
  transition: true,
  keyComponents: true,
  combos: true,
} as const;
