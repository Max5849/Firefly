import { TEMP_EXPONENTS } from '../hand/useHand';

/**
 * The inverse temperature every node is asked for at, per pot: ln(2^k) for k one of
 * TEMP_EXPONENTS. At ln(2^k) an option worth one pot more is played 2^k times as often;
 * k = 20 is training's INV_TEMP. It sets both the policy shown at the spot and the one
 * the ranges are replayed through. [ and ] step it from the keyboard.
 */
export function TempControl({ step, onChange }: { step: number; onChange: (step: number) => void }) {
  return (
    <div className="flex items-center gap-2 text-[12px]" title="inverse temperature per pot - [ and ] step it">
      <span className="text-[10px] tracking-[0.14em] uppercase" style={{ color: 'var(--color-ash)' }}>
        temp
      </span>
      {TEMP_EXPONENTS.map((exponent, i) => {
        const on = i === step;
        return (
          <button
            key={exponent}
            type="button"
            // The page is played from the keyboard, so a button must not take focus.
            onMouseDown={(event) => event.preventDefault()}
            onClick={(event) => {
              event.currentTarget.blur();
              onChange(i);
            }}
            className="rounded-[3px] border px-2 py-[3px] text-[11px]"
            style={{
              borderColor: on ? 'var(--color-chalk)' : 'var(--color-rule)',
              color: on ? 'var(--color-chalk)' : 'var(--color-ash)',
            }}
            title={`ln 2^${exponent} = ${(exponent * Math.LN2).toFixed(3)}`}
          >
            2<sup>{exponent}</sup>
          </button>
        );
      })}
    </div>
  );
}
