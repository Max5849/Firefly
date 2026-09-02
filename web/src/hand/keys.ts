import { RANKS, WIRE_SUITS } from '../grid/comboIndex';

/** Card-entry keys. 'a' is also all-in and 'c' also call, but the modes are disjoint. */
export function rankFromKey(key: string): string | null {
  if (key >= '2' && key <= '9') return key;
  const upper = key.toUpperCase();
  return 'TJQKA'.includes(upper) && RANKS.includes(upper) ? upper : null;
}

export function suitFromKey(key: string): string | null {
  return WIRE_SUITS.includes(key) ? key : null;
}

/** Option index for an action key, or -1. */
export function optionFromKey(key: string): number {
  return ['f', 'c', 's', 'b', 'a'].indexOf(key);
}

export function cardCount(board: string): number {
  return board.length / 2;
}
