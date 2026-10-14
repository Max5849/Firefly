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
