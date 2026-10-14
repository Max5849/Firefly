import type { ConfigWithSpec, MetaResponse, StrategyRequest, StrategyResponse } from './types';

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
 * The outcomes worth acting on rather than merely reporting, read from the error
 * body's `code`. A street rollover is announced as needsBoard, since the server
 * cannot go on without the cards.
 */
export type ApiFailure =
  | { kind: 'needsBoard'; street: string; needed: number; message: string }
  | { kind: 'handEnded'; actionIndex: number; endedBy: string; message: string }
  | { kind: 'illegal'; actionIndex: number; reason: string; message: string }
  | {
      kind: 'strategyMissing';
      spec: string;
      path: string;
      available: ConfigWithSpec[];
      message: string;
    }
  | { kind: 'other'; message: string };

export class ApiError extends Error {
  readonly failure: ApiFailure;
  constructor(failure: ApiFailure) {
    super(failure.message);
    this.name = 'ApiError';
    this.failure = failure;
  }
}

function classify(body: Record<string, unknown>): ApiFailure {
  const message = typeof body.error === 'string' ? body.error : 'the request failed';
  const actionIndex = typeof body.actionIndex === 'number' ? body.actionIndex : -1;
  switch (body.code) {
    case 'needsBoard':
      return {
        kind: 'needsBoard',
        street: String(body.street ?? 'flop'),
        needed: typeof body.cards === 'number' ? body.cards : 3,
        message,
      };
    case 'handEnded':
      return { kind: 'handEnded', actionIndex, endedBy: String(body.endedBy ?? ''), message };
    case 'illegal':
      return {
        kind: 'illegal',
        actionIndex,
        reason: typeof body.reason === 'string' ? body.reason : message,
        message,
      };
    case 'strategyMissing':
      return {
        kind: 'strategyMissing',
        spec: String(body.spec ?? ''),
        path: String(body.path ?? ''),
        available: Array.isArray(body.available) ? (body.available as ConfigWithSpec[]) : [],
        message,
      };
    default:
      return { kind: 'other', message };
  }
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
