import { useEffect, useMemo, useRef, useState } from 'react';
import type { Combo, StrategyResponse } from '../api/types';
import { BLACK_HEX, OPTION_HEX, SLATE_HEX, VOID_HEX } from '../hand/seats';
import { boardSuitCounts } from './comboIndex';
import { CELLS } from './layout';
import { orderCell } from './order';

/**
 * A range chart is only readable square, and aspect-ratio alone cannot hold that
 * against a flex parent constrained on both axes - whichever of width or height
 * binds wins and the square is lost. So the side is measured and applied.
 */
function useSquareSide(): [React.RefObject<HTMLDivElement | null>, number] {
  const ref = useRef<HTMLDivElement>(null);
  const [side, setSide] = useState(0);

  useEffect(() => {
    const element = ref.current;
    if (!element) return;
    const observer = new ResizeObserver(([entry]) => {
      const { width, height } = entry.contentRect;
      setSide(Math.max(0, Math.floor(Math.min(width, height))));
    });
    observer.observe(element);
    return () => observer.disconnect();
  }, []);

  return [ref, side];
}

/**
 * Three ways a column carries nothing to read, all drawn black: the combination
 * uses a board card, the solver never built that node, or its meta fell under the
 * 5% cut here or on any earlier street.
 */
function makeEliminated(combos: Combo[] | null, black: Set<number>) {
  return (index: number): boolean => {
    if (!combos) return true;
    const row = combos[index];
    if (!row?.found || !row.frequencies) return true;
    return black.has(index);
  };
}

/**
 * Colour is drawn on one canvas rather than as 1326 elements. Sub-pixel geometry
 * is the reason: at fractional browser zoom, adjacent DOM boxes round
 * independently and leave hairline seams between them, which read as dividers
 * that were never asked for. On a canvas every boundary is computed once and
 * rounded to a whole device pixel, so neighbours share an exact edge.
 *
 * The labels are the exception and are DOM - see the note on `labels` below.
 */
export function RangeGrid({
  response,
  black,
  onHover,
}: {
  response: StrategyResponse | null;
  /** Combinations thin at this node or any earlier one in the hand. */
  black: Set<number>;
  /** The combination under the cursor, or null when it leaves the grid. */
  onHover: (combo: number | null) => void;
}) {
  const [ref, side] = useSquareSide();
  const canvas = useRef<HTMLCanvasElement>(null);
  // What was actually drawn in each cell, left to right. Hit-testing reads this
  // rather than recomputing the order, so the readout cannot disagree with the
  // picture if the ordering rule ever changes.
  const drawn = useRef<number[][]>([]);
  // Kept in state so the label layer sits on exactly the boundaries the canvas
  // drew, including after a move to a display with a different ratio.
  const [metrics, setMetrics] = useState({ pixels: 0, dpr: 1 });

  const combos = response?.combos ?? null;

  useEffect(() => {
    const element = canvas.current;
    if (!element || side <= 0) return;
    const context = element.getContext('2d');
    if (!context) return;

    // Everything below is measured in DEVICE pixels, with no context transform.
    //
    // The alternative - a CSS-pixel coordinate system scaled by setTransform -
    // goes soft on any display whose ratio is not a whole number. macOS scaled
    // modes report 1.7999999523: round(side * dpr) then does not divide back to
    // an integer number of CSS pixels, the browser resamples the whole canvas to
    // cover the difference, and every edge on it blurs.
    const dpr = window.devicePixelRatio || 1;
    const pixels = Math.round(side * dpr);
    element.width = pixels;
    element.height = pixels;
    element.style.width = `${pixels / dpr}px`;
    element.style.height = `${pixels / dpr}px`;
    context.setTransform(1, 0, 0, 1, 0, 0);
    setMetrics((current) =>
      current.pixels === pixels && current.dpr === dpr ? current : { pixels, dpr },
    );

    const board = boardSuitCounts(response?.node?.board ?? '');
    const eliminated = makeEliminated(combos, black);

    context.fillStyle = VOID_HEX;
    context.fillRect(0, 0, pixels, pixels);

    const bound = (i: number) => Math.round((i * pixels) / 13);

    for (let row = 0; row < 13; row++) {
      for (let column = 0; column < 13; column++) {
        const cell = CELLS[row * 13 + column];
        const x = bound(column);
        const y = bound(row);
        const width = bound(column + 1) - x;
        const height = bound(row + 1) - y;

        context.fillStyle = SLATE_HEX;
        context.fillRect(x, y, width, height);

        const order = combos ? orderCell(cell.strips, board, eliminated) : cell.strips;
        drawn.current[row * 13 + column] = order;
        const columns = order.length;

        for (let index = 0; index < columns; index++) {
          // Boundaries are rounded, then shared: column n ends exactly where
          // column n+1 begins, so no gap can open up at any zoom level.
          const x0 = x + Math.round((index * width) / columns);
          const x1 = x + Math.round(((index + 1) * width) / columns);
          const combo = order[index];

          if (eliminated(combo)) {
            context.fillStyle = BLACK_HEX;
            context.fillRect(x0, y, x1 - x0, height);
            continue;
          }

          // Bands stack from the bottom: fold, call, raise, raise, all in.
          const frequencies = combos![combo]!.frequencies!;
          let filled = 0;
          for (let option = 0; option < 5; option++) {
            const share = frequencies[option];
            if (!(share > 0)) continue;
            const from = Math.round(filled * height);
            filled = Math.min(1, filled + share);
            const to = Math.round(filled * height);
            if (to <= from) continue;
            context.fillStyle = OPTION_HEX[option];
            context.fillRect(x0, y + height - to, x1 - x0, to - from);
          }
        }
      }
    }
  }, [side, response, black, combos]);

  /**
   * Labels are DOM, not canvas.
   *
   * The face is already identical - canvas measures "AKs" at 21.600px against
   * the left column's 21.609, where generic monospace measures 21.674 - so the
   * font string was never the problem. What differs is
   * `-webkit-font-smoothing: antialiased`, which the page sets on body and which
   * applies only to DOM text. Canvas has its own text pipeline and ignores it,
   * so the same face at the same size renders heavier and softer there than it
   * does three inches to the left. Rendering the labels as DOM is the only way
   * they match the rest of the interface exactly.
   */
  const labels = useMemo(() => {
    const { pixels, dpr } = metrics;
    if (pixels <= 0) return [];
    const eliminated = makeEliminated(combos, black);
    const bound = (i: number) => Math.round((i * pixels) / 13) / dpr;

    return CELLS.map((cell, index) => ({
      label: cell.label,
      left: bound(index % 13),
      top: bound(Math.floor(index / 13)),
      painted: combos !== null && cell.strips.some((combo) => !eliminated(combo)),
    }));
  }, [metrics, combos, black]);

  /**
   * Pixel -> combination. The canvas is a single element, so there is no node
   * under the cursor to identify; the only way back is to run the layout in
   * reverse. Boundaries use the same rounded arithmetic the draw did, and the
   * column order comes from what was drawn rather than being recomputed.
   */
  function comboAt(clientX: number, clientY: number, rect: DOMRect): number | null {
    const { pixels, dpr } = metrics;
    if (pixels <= 0) return null;
    const px = (clientX - rect.left) * dpr;
    const py = (clientY - rect.top) * dpr;
    if (px < 0 || py < 0 || px >= pixels || py >= pixels) return null;

    const bound = (i: number) => Math.round((i * pixels) / 13);
    const find = (value: number) => {
      let i = Math.min(12, Math.max(0, Math.floor((value * 13) / pixels)));
      // Rounding can put the estimate one cell out at a boundary.
      while (i > 0 && value < bound(i)) i--;
      while (i < 12 && value >= bound(i + 1)) i++;
      return i;
    };

    const column = find(px);
    const row = find(py);
    const order = drawn.current[row * 13 + column];
    if (!order || order.length === 0) return null;

    const x = bound(column);
    const width = bound(column + 1) - x;
    const columns = order.length;
    let index = Math.min(columns - 1, Math.max(0, Math.floor(((px - x) * columns) / width)));
    while (index > 0 && px - x < Math.round((index * width) / columns)) index--;
    while (index < columns - 1 && px - x >= Math.round(((index + 1) * width) / columns)) index++;
    return order[index];
  }

  const size = metrics.pixels > 0 ? metrics.pixels / metrics.dpr : 0;

  return (
    <div ref={ref} className="flex min-h-0 min-w-0 flex-1 items-center justify-center p-5">
      <div
        className="relative"
        style={{ width: size, height: size }}
        onMouseMove={(event) =>
          onHover(comboAt(event.clientX, event.clientY, event.currentTarget.getBoundingClientRect()))
        }
        onMouseLeave={() => onHover(null)}
      >
        <canvas ref={canvas} className="block" role="img" aria-label="range grid" />
        <div className="pointer-events-none absolute inset-0 select-none">
          {labels.map((entry) => (
            <span
              key={entry.label}
              className="absolute text-[12px] leading-none"
              style={{
                left: entry.left + 3,
                top: entry.top + 3,
                color: entry.painted ? '#ffffff' : 'var(--color-ash)',
              }}
            >
              {entry.label}
            </span>
          ))}
        </div>
      </div>
    </div>
  );
}
