export function formatChips(value: number): string {
  return Number.isInteger(value) ? String(value) : value.toFixed(1);
}

/** Node totals run from the hundreds on a deep river to billions preflop. */
export function formatVisits(value: number): string {
  if (value >= 1e9) return `${(value / 1e9).toFixed(2)}B`;
  if (value >= 1e6) return `${(value / 1e6).toFixed(2)}M`;
  if (value >= 1e3) return `${(value / 1e3).toFixed(1)}k`;
  return String(Math.round(value));
}

/** Chips relative to folding, signed so a loss never reads as a win. */
export function formatEv(value: number): string {
  const sign = value > 0 ? '+' : value < 0 ? '−' : '';
  const magnitude = Math.abs(value);
  return sign + (magnitude >= 100 ? magnitude.toFixed(0) : magnitude.toFixed(2));
}

export function formatShare(value: number): string {
  return `${(value * 100).toFixed(value > 0 && value < 0.001 ? 2 : 1)}%`;
}
