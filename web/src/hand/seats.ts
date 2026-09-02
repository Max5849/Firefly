/** Fallback seat names; /meta supplies the real list at runtime. */
export const SEAT_NAMES = ['BTN', 'SB', 'BB', 'UTG', 'UTG+1', 'MP', 'CO'];

export const OPTION_NAMES = ['fold', 'call', 'raise 0.5x', 'raise 1.5x', 'all in'];

/** The keys that play each option, in option-index order. */
export const OPTION_KEYS = ['f', 'c', 's', 'b', 'a'];

export const OPTION_COLORS = [
  'var(--color-act-fold)',
  'var(--color-act-call)',
  'var(--color-act-small)',
  'var(--color-act-big)',
  'var(--color-act-shove)',
];

export const STREETS = ['preflop', 'flop', 'turn', 'river'];
