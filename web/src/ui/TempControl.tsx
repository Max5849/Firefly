/**
 * The inverse temperature every node is asked for at, as ln(2^k). It sets both the
 * policy shown at the spot and the one the ranges are replayed through; k = 8 is the
 * temperature training ran at. [ and ] step it from the keyboard.
 */
export function TempControl({
  exponent,
  training,
  limits,
  onChange,
}: {
  exponent: number;
  training: number;
  limits: [number, number];
  onChange: (exponent: number) => void;
}) {
  const value = exponent * Math.LN2;
  return (
    <label className="flex items-center gap-2 text-[12px]" title="inverse temperature - [ and ] step it">
      <span className="text-[10px] tracking-[0.14em] uppercase" style={{ color: 'var(--color-ash)' }}>
        temp
      </span>
      <input
        type="range"
        min={limits[0]}
        max={limits[1]}
        step={1}
        value={exponent}
        onChange={(event) => onChange(Number(event.target.value))}
        // The page is played from the keyboard, so the slider must not keep focus.
        onPointerUp={(event) => event.currentTarget.blur()}
        onKeyUp={(event) => event.currentTarget.blur()}
        className="w-28 accent-[var(--color-chalk)]"
      />
      <span className="w-[9.5rem] whitespace-nowrap">
        ln 2<sup>{exponent}</sup> = {value.toFixed(2)}
        <span style={{ color: 'var(--color-ash)' }}>{exponent === training ? ' · trained' : ''}</span>
      </span>
    </label>
  );
}
