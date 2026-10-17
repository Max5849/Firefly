import { invTempOf } from '../hand/useHand';

/** The exponent of 2 a step stands for, short: 0.125, 1.41, 8. */
function formatExponent(step: number): string {
  const exponent = Math.pow(2, step / 8);
  return Number.isInteger(exponent) ? String(exponent) : String(+exponent.toFixed(3));
}

/**
 * The inverse temperature every node is asked for at, per chip of EV: ln(2^k), with k
 * from 1/8 to 8 in steps of 2^(1/8). At ln(2^k) an option one chip better is played
 * 2^k times as often. It sets both the policy shown at the spot and the one the ranges
 * are replayed through. [ and ] step it from the keyboard.
 */
export function TempControl({
  step,
  limits,
  onChange,
}: {
  step: number;
  limits: [number, number];
  onChange: (step: number) => void;
}) {
  return (
    <label
      className="flex items-center gap-2 text-[12px]"
      title="inverse temperature per chip of EV - [ and ] step it"
    >
      <span className="text-[10px] tracking-[0.14em] uppercase" style={{ color: 'var(--color-ash)' }}>
        temp
      </span>
      <input
        type="range"
        min={limits[0]}
        max={limits[1]}
        step={1}
        value={step}
        onChange={(event) => onChange(Number(event.target.value))}
        // The page is played from the keyboard, so the slider must not keep focus.
        onPointerUp={(event) => event.currentTarget.blur()}
        onKeyUp={(event) => event.currentTarget.blur()}
        className="w-28 accent-[var(--color-chalk)]"
      />
      <span className="w-[9.5rem] whitespace-nowrap">
        ln 2<sup>{formatExponent(step)}</sup> = {invTempOf(step).toFixed(3)}
      </span>
    </label>
  );
}
