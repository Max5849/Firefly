import type { SpotSection } from '../api/types';

/**
 * A key is shared by every combination in its cluster, so a record's visits are
 * the cluster's. The count compared here is normalised per combination - visits
 * divided by the cluster's size - because a cluster covering 495 holdings would
 * otherwise always outrank one covering 6.
 */
export interface MetaStats {
  /** Combination indices whose cluster is too thin to read here. */
  black: Set<number>;
  /** The largest normalised count at this node, which sets the cut. */
  peak: number;
}

/** A cluster below this share of the busiest one carries too little to read. */
const CUT = 0.05;

export function metaStats(spot: SpotSection | undefined): MetaStats | null {
  const combos = spot?.combos;
  if (!combos) return null;

  const normalised = new Map<string, number>();
  for (const row of combos) {
    if (row?.found && typeof row.visits === 'number' && row.clusterCombos > 0) {
      normalised.set(row.key, row.visits / row.clusterCombos);
    }
  }

  let peak = 0;
  for (const visits of normalised.values()) if (visits > peak) peak = visits;

  const black = new Set<number>();
  for (let i = 0; i < combos.length; i++) {
    const row = combos[i];
    if (!row?.found) continue;
    const visits = normalised.get(row.key);
    if (visits !== undefined && visits < CUT * peak) black.add(i);
  }

  return { black, peak };
}
