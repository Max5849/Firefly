/**
 * The keycap row is the legality display. Only legal options appear, so the thing
 * that tells you what you may do is the same thing that tells you which key does it.
 */
export function Keycap({ label, tint }: { label: string; tint?: string }) {
  return (
    <span
      className="inline-flex h-6 min-w-6 items-center justify-center rounded-[3px] border px-1.5 text-[11px] font-semibold uppercase"
      style={{
        borderColor: tint ?? 'var(--color-rule)',
        color: tint ?? 'var(--color-ash)',
      }}
    >
      {label}
    </span>
  );
}
