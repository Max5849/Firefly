import { useEffect, useMemo, useRef, useState } from 'react';
import type { Combo } from '../api/types';
import { OPTION_COLORS } from '../hand/seats';
import { CELLS } from './layout';

/**
 * A range chart is only readable square, and aspect-ratio alone cannot hold that
 * against a flex parent that is wide in one direction and short in the other -
 * whichever of width or height is constrained wins and the square is lost. So the
 * side is measured and applied.
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
 * One strip is one of the 1326 combinations, drawn as a single element carrying a
 * five-band gradient. Bands stack from the bottom: fold, call, small, big, shove.
 */
function gradient(frequencies: number[]): string {
  const stops: string[] = [];
  let at = 0;
  for (let option = 0; option < 5; option++) {
    const share = frequencies[option];
    if (!(share > 0)) continue;
    const to = Math.min(100, at + share * 100);
    stops.push(`${OPTION_COLORS[option]} ${at.toFixed(3)}% ${to.toFixed(3)}%`);
    at = to;
  }
  if (stops.length === 0) return 'none';
  return `linear-gradient(to top, ${stops.join(', ')})`;
}

interface StripStyle {
  background: string;
  opacity: number;
}

export function RangeGrid({ combos }: { combos: Combo[] | null }) {
  const styles = useMemo<StripStyle[] | null>(() => {
    if (!combos) return null;

    // Opacity is linear from the busiest holding of the 1326. Impossible rows
    // (null), meta misses and table misses all carry no visits and land at 0.
    let peak = 0;
    for (const row of combos) {
      if (row?.found && typeof row.visits === 'number' && row.visits > peak) {
        peak = row.visits;
      }
    }

    return combos.map((row) => {
      if (!row?.found || !row.frequencies) return { background: 'none', opacity: 0 };
      const visits = typeof row.visits === 'number' ? row.visits : 0;
      return {
        background: gradient(row.frequencies),
        opacity: peak > 0 ? visits / peak : 0,
      };
    });
  }, [combos]);

  const [ref, side] = useSquareSide();

  return (
    <div ref={ref} className="flex min-h-0 min-w-0 flex-1 items-center justify-center p-5">
      <div
        className="grid gap-px"
        style={{
          width: side,
          height: side,
          background: 'var(--color-void)',
          gridTemplateColumns: 'repeat(13, minmax(0, 1fr))',
          gridTemplateRows: 'repeat(13, minmax(0, 1fr))',
        }}
      >
        {CELLS.map((cell) => {
          // A cell whose every combination is blocked by the board, or was never
          // explored, has nothing behind its label - so the label lifts back off
          // the dark ink and onto the muted grey.
          const painted = styles !== null && cell.strips.some((i) => styles[i].opacity > 0);
          return (
          <div
            key={cell.label}
            className="relative min-h-0 min-w-0 overflow-hidden"
            style={{ background: 'var(--color-slate)' }}
          >
            {/* Strips butt together with no divider; only the cell gap is drawn. */}
            <div className="absolute inset-0 flex" aria-hidden="true">
              {cell.strips.map((index) => (
                <div
                  key={index}
                  className="strip h-full flex-1"
                  style={styles ? styles[index] : { background: 'none', opacity: 0 }}
                />
              ))}
            </div>
            {/* Dark ink reads on every action colour. */}
            <span
              className="pointer-events-none absolute top-[2px] left-[3px] text-[9px] leading-none font-semibold"
              style={{ color: painted ? 'rgba(10, 11, 13, 0.78)' : 'var(--color-ash)' }}
            >
              {cell.label}
            </span>
          </div>
          );
        })}
      </div>
    </div>
  );
}
