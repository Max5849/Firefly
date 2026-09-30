import { COMBO_CARDS, rankOf, suitOf } from './comboIndex';

/**
 * How far a combination is toward a flush in one suit, and which card of the
 * hand carries it.
 *
 * Only suits the hole cards actually contribute to are considered. Counting every
 * suit would hand each combination the board's own longest suit, which is shared
 * by the whole cell and so orders nothing.
 */
interface Dominant {
  /** Board cards plus hole cards of that suit. */
  total: number;
  /** Rank index of the best hole card in that suit - 0 is the ace. */
  bestRank: number;
  /** Rank index of the other hole card in that suit, or 13 if there is none. */
  secondRank: number;
  /** 3 = spades .. 0 = clubs, used only to break a total tie. */
  suit: number;
}

function dominantSuit(combo: number, board: number[]): Dominant {
  const [first, second] = COMBO_CARDS[combo];
  const suits = suitOf(first) === suitOf(second) ? [suitOf(first)] : [suitOf(first), suitOf(second)];

  let best: Dominant | null = null;
  for (const suit of suits) {
    const ranks: number[] = [];
    if (suitOf(first) === suit) ranks.push(rankOf(first));
    if (suitOf(second) === suit) ranks.push(rankOf(second));
    ranks.sort((a, b) => a - b);

    const candidate: Dominant = {
      total: board[suit] + ranks.length,
      bestRank: ranks[0],
      secondRank: ranks.length > 1 ? ranks[1] : 13,
      suit,
    };
    if (
      best === null ||
      candidate.total > best.total ||
      (candidate.total === best.total && candidate.bestRank < best.bestRank) ||
      (candidate.total === best.total &&
        candidate.bestRank === best.bestRank &&
        candidate.suit > best.suit)
    ) {
      best = candidate;
    }
  }
  return best as Dominant;
}

/**
 * Left to right: the most suited first, and within an equal count the one whose
 * card in that suit is highest. On a two-spade flop that puts A♠5x ahead of Ax5♠,
 * both of which hold three spades. Eliminated combinations fall to the right, so
 * the live columns stay packed against the left edge of the cell.
 */
export function orderCell(
  combos: number[],
  board: number[],
  eliminated: (combo: number) => boolean,
): number[] {
  return combos
    .map((combo) => ({ combo, dead: eliminated(combo), rank: dominantSuit(combo, board) }))
    .sort((a, b) => {
      if (a.dead !== b.dead) return a.dead ? 1 : -1;
      if (a.rank.total !== b.rank.total) return b.rank.total - a.rank.total;
      if (a.rank.bestRank !== b.rank.bestRank) return a.rank.bestRank - b.rank.bestRank;
      if (a.rank.suit !== b.rank.suit) return b.rank.suit - a.rank.suit;
      if (a.rank.secondRank !== b.rank.secondRank) return a.rank.secondRank - b.rank.secondRank;
      return a.combo - b.combo;
    })
    .map((entry) => entry.combo);
}
