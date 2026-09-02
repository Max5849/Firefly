import { SUIT_PIPS, WIRE_SUITS } from '../grid/comboIndex';

const TILE: Record<string, string> = {
  c: 'var(--color-suit-c)',
  d: 'var(--color-suit-d)',
  h: 'var(--color-suit-h)',
  s: 'var(--color-suit-s)',
};

/** A flat suit-coloured tile with the rank and pip knocked out in near-black. */
export function Card({ text }: { text: string }) {
  const rank = text[0].toUpperCase();
  const suit = text[1].toLowerCase();
  const pip = SUIT_PIPS[WIRE_SUITS.indexOf(suit)] ?? '';

  return (
    <span
      className="inline-flex h-7 w-6 flex-col items-center justify-center rounded-[2px] leading-none"
      style={{ background: TILE[suit] ?? 'var(--color-ash)', color: 'var(--color-void)' }}
      aria-label={`${rank}${pip}`}
    >
      <span className="text-[11px] font-semibold">{rank}</span>
      <span className="text-[10px]">{pip}</span>
    </span>
  );
}

/** Splits "As7h2d" into tiles. Slots render as outlines while a board is typed. */
export function CardRow({ board, slots = 0 }: { board: string; slots?: number }) {
  const cards: string[] = [];
  for (let i = 0; i + 1 < board.length; i += 2) {
    cards.push(board.slice(i, i + 2));
  }
  const empty = Math.max(0, slots - cards.length);

  return (
    <span className="flex gap-1">
      {cards.map((text, i) => (
        <Card key={`${text}-${i}`} text={text} />
      ))}
      {Array.from({ length: empty }, (_, i) => (
        <span
          key={`slot-${i}`}
          className="inline-block h-7 w-6 rounded-[2px] border border-dashed"
          style={{ borderColor: 'var(--color-rule)' }}
        />
      ))}
    </span>
  );
}
