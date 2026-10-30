export function formatChips(value: number): string {
  return Number.isInteger(value) ? String(value) : value.toFixed(1);
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
