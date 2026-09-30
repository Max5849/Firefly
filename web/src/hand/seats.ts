/**
 * Seat 0 is the button and the engine deals preflop from seat 3. /meta calls
 * seats 4 and 5 UTG+1 and MP; these are the same seats under the names actually
 * used at the table, so this list wins over the one the server sends.
 */
export const SEAT_NAMES = ['BTN', 'SB', 'BB', 'UTG', 'LJ', 'HJ', 'CO'];

/** No sizing in the name - the chip total is shown beside it instead. */
export const OPTION_NAMES = ['fold', 'call', 'raise', 'raise', 'all in'];

/** The keys that play each option, in option-index order. */
export const OPTION_KEYS = ['f', 'c', 's', 'b', 'a'];

export const OPTION_COLORS = [
  'var(--color-act-fold)',
  'var(--color-act-call)',
  'var(--color-act-small)',
  'var(--color-act-big)',
  'var(--color-act-shove)',
];

/** The same five, resolved - the canvas cannot read custom properties. */
export const OPTION_HEX = ['#3b82f6', '#22c55e', '#ff6fb5', '#f43f3f', '#b558f6'];
/** Every eliminated combination, whatever eliminated it. */
export const BLACK_HEX = '#000000';
export const SLATE_HEX = '#14161a';
export const VOID_HEX = '#0a0b0d';

/** Three fixed inverse temperatures, around the value training finished at. */
export const LN65536 = Math.log(65536);
export const TEMPS: { label: string; value: number }[] = [
  { label: 'low', value: LN65536 / 4 },
  { label: 'med', value: LN65536 },
  { label: 'high', value: LN65536 * 4 },
];
