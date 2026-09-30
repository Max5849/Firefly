import { COMBO_COUNT } from '../api/types';
import type { StrategyResponse } from '../api/types';

/**
 * A hand reaching the current node with under this probability is not really in
 * the player's range any more. Absolute, not relative: the value is a
 * probability, so it has a scale of its own.
 */
export const REACH_CUT = 0.01;

/**
 * How likely each combination is to still be in a seat's range, as a float in
 * 0..1.
 *
 * Every combination starts at 1 and is multiplied by the odds it did not fold,
 * once for each decision that seat has made. The running product is the chance
 * that holding played the line it did rather than folding somewhere along it.
 *
 * Only the seat's own decisions count - the actions submitted to the API.
 * `frames[i]` records the action that produced it, and the node that action was
 * taken at is `frames[i - 1]`, whose combos are that seat's own strategy.
 */
export function reachProbabilities(
  frames: { response: StrategyResponse }[],
): Map<number, Float64Array> {
  const reach = new Map<number, Float64Array>();

  for (let i = 1; i < frames.length; i++) {
    const acted = frames[i].response.transition?.lastAction;
    const decision = frames[i - 1].response;
    if (!acted || decision.node?.seat !== acted.seat) continue;

    let values = reach.get(acted.seat);
    if (!values) {
      values = new Float64Array(COMBO_COUNT).fill(1);
      reach.set(acted.seat, values);
    }

    const combos = decision.combos;
    if (!combos) continue;
    for (let index = 0; index < combos.length; index++) {
      const row = combos[index];
      // A board collision or an unbuilt node says nothing about whether this
      // holding folds, so it leaves the running product alone rather than
      // collapsing it to zero.
      if (!row?.found || !row.frequencies) continue;
      // frequencies[0] is fold, and is exactly 0 where folding is not legal.
      values[index] *= 1 - row.frequencies[0];
    }
  }

  return reach;
}

/** The combinations whose reach probability has fallen under the cut. */
export function belowCut(values: Float64Array | undefined): Set<number> {
  const dropped = new Set<number>();
  if (!values) return dropped;
  for (let index = 0; index < values.length; index++) {
    if (values[index] < REACH_CUT) dropped.add(index);
  }
  return dropped;
}
