import type { StrategyResponse } from '../api/types';

/**
 * A key is shared by every combination with the same hand meta, so the node's
 * real visit count is the sum over *distinct* keys. Adding up the per-combo
 * figures instead would multiply each meta by the size of its cluster.
 */
export interface MetaStats {
  /** Total visits at this node: the sum of the distinct metas. */
  total: number;
  /**
   * Combination indices whose meta is too thin to read here. The count compared
   * is normalised per combination - nodeVisits / metaCombos, which the API
   * reports as `visits` - because a meta covering 495 holdings would otherwise
   * always outrank one covering 6.
   */
  black: Set<number>;
  /** The largest normalised count at this node, which sets the cut. */
  peak: number;
}

/** A meta below this share of the busiest one carries too little to read. */
const CUT = 0.05;

export function metaStats(response: StrategyResponse | undefined): MetaStats | null {
  const combos = response?.combos;
  if (!combos) return null;

  const normalised = new Map<string, number>();
  const raw = new Map<string, number>();
  for (const row of combos) {
    if (row?.found && row.key !== undefined && typeof row.nodeVisits === 'number') {
      raw.set(row.key, row.nodeVisits);
      if (typeof row.visits === 'number') normalised.set(row.key, row.visits);
    }
  }

  let total = 0;
  for (const visits of raw.values()) total += visits;

  let peak = 0;
  for (const visits of normalised.values()) if (visits > peak) peak = visits;

  const thinKeys = new Set<string>();
  for (const [key, visits] of normalised) {
    if (visits < CUT * peak) thinKeys.add(key);
  }

  const black = new Set<number>();
  for (let i = 0; i < combos.length; i++) {
    const row = combos[i];
    if (row?.found && row.key !== undefined && thinKeys.has(row.key)) black.add(i);
  }

  return { total, black, peak };
}
