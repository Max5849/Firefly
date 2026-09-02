// The wire encoding, from /meta:
//   card  = rank * 4 + suit,  rank 0 = ace descending to 12 = deuce,  suits "cdhs"
//   index = i * (101 - i) / 2 + j - 1  for cards i < j
// Because rank 0 is the ace, a numerically smaller card is the *stronger* one.

export const RANKS = 'AKQJT98765432';
/** Suit characters in wire order: index 0 = clubs .. 3 = spades. */
export const WIRE_SUITS = 'cdhs';
/** Suit indices in the left-to-right display order s, h, d, c. */
export const DISPLAY_SUITS = [3, 2, 1, 0] as const;

export const SUIT_PIPS = ['♣', '♦', '♥', '♠']; // c d h s

export function card(rank: number, suit: number): number {
  return rank * 4 + suit;
}

export function rankOf(c: number): number {
  return Math.floor(c / 4);
}

export function suitOf(c: number): number {
  return c % 4;
}

/** Contiguous over 0..1325. Order of arguments does not matter. */
export function comboIndex(a: number, b: number): number {
  const i = Math.min(a, b);
  const j = Math.max(a, b);
  return (i * (101 - i)) / 2 + j - 1;
}

export function cardName(c: number): string {
  return RANKS[rankOf(c)] + WIRE_SUITS[suitOf(c)];
}

/** Reads high card first, matching the server's own `combo` strings. */
export function comboName(a: number, b: number): string {
  const i = Math.min(a, b);
  const j = Math.max(a, b);
  return cardName(i) + cardName(j);
}
