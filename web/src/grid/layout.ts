import { COMBO_COUNT } from '../api/types';
import { DISPLAY_SUITS, RANKS, card, comboIndex } from './comboIndex';

export type CellKind = 'pair' | 'suited' | 'offsuit';

export interface Cell {
  label: string;
  kind: CellKind;
  /**
   * Wire indices into combos[], as one row of columns. The order here is only a
   * canonical starting set - display order depends on the board and on which
   * combinations are eliminated, so it is computed per node by orderCell().
   */
  strips: number[];
}

/**
 * The 169 cells of a range chart, row and column running A..2.
 *   diagonal    pairs,   6 strips
 *   above       suited,  4 strips
 *   below       offsuit, 12 strips
 *
 * The wire orders suits c,d,h,s; the display order is s,h,d,c, so every strip
 * list is built from DISPLAY_SUITS rather than by sorting wire indices.
 * Offsuit cells are high-suit-major, low-suit-minor.
 */
function buildCells(): Cell[] {
  const cells: Cell[] = [];

  for (let row = 0; row < 13; row++) {
    for (let col = 0; col < 13; col++) {
      if (row === col) {
        const strips: number[] = [];
        for (let a = 0; a < DISPLAY_SUITS.length; a++) {
          for (let b = a + 1; b < DISPLAY_SUITS.length; b++) {
            strips.push(comboIndex(card(row, DISPLAY_SUITS[a]), card(row, DISPLAY_SUITS[b])));
          }
        }
        cells.push({ label: RANKS[row] + RANKS[row], kind: 'pair', strips });
        continue;
      }

      // Rank 0 is the ace, so the smaller index is the higher card.
      const high = Math.min(row, col);
      const low = Math.max(row, col);
      const label = RANKS[high] + RANKS[low];

      if (row < col) {
        const strips = DISPLAY_SUITS.map((suit) => comboIndex(card(high, suit), card(low, suit)));
        cells.push({ label: `${label}s`, kind: 'suited', strips });
      } else {
        const strips: number[] = [];
        for (const highSuit of DISPLAY_SUITS) {
          for (const lowSuit of DISPLAY_SUITS) {
            if (highSuit === lowSuit) continue;
            strips.push(comboIndex(card(high, highSuit), card(low, lowSuit)));
          }
        }
        cells.push({ label: `${label}o`, kind: 'offsuit', strips });
      }
    }
  }

  return cells;
}

export const CELLS: Cell[] = buildCells();

// 13*6 + 78*4 + 78*12 = 1326. A duplicated or dropped strip would otherwise show
// up as a plausible-looking grid rather than as an error.
{
  const seen = new Set<number>();
  for (const cell of CELLS) {
    for (const index of cell.strips) {
      if (index < 0 || index >= COMBO_COUNT) {
        throw new Error(`combo index ${index} out of range in cell ${cell.label}`);
      }
      if (seen.has(index)) {
        throw new Error(`combo index ${index} appears twice, at cell ${cell.label}`);
      }
      seen.add(index);
    }
  }
  if (CELLS.length !== 169 || seen.size !== COMBO_COUNT) {
    throw new Error(`grid covers ${seen.size} combos across ${CELLS.length} cells`);
  }
}
