import type { GameSection, KeyField, KeySection, SpotSection } from '../api/types';
import { COMBO_CARDS, comboName } from '../grid/comboIndex';
import { OPTION_COLORS } from '../hand/seats';
import { formatEv, formatShare } from './format';

function Heading({ children, aside }: { children: React.ReactNode; aside?: React.ReactNode }) {
  return (
    <div className="mb-1.5 flex items-baseline justify-between">
      <span className="text-[10px] tracking-[0.16em] uppercase" style={{ color: 'var(--color-ash)' }}>
        {children}
      </span>
      {aside ? <span className="text-[10px]" style={{ color: 'var(--color-ash)' }}>{aside}</span> : null}
    </div>
  );
}

function Row({ label, children, dim }: { label: string; children: React.ReactNode; dim?: boolean }) {
  return (
    <div className="flex items-baseline justify-between gap-2 py-[1px] text-[11px]">
      <span style={{ color: 'var(--color-ash)' }}>{label}</span>
      <span className="truncate text-right" style={{ color: dim ? 'var(--color-ash)' : undefined }}>
        {children}
      </span>
    </div>
  );
}

/** A horizontal stacked bar of the five actions, in grid order. */
function MixBar({ mix }: { mix: number[] }) {
  return (
    <div className="flex h-2 w-full overflow-hidden rounded-[2px]" style={{ background: 'var(--color-void)' }}>
      {mix.map((share, option) =>
        share > 0 ? (
          <span key={option} style={{ width: `${share * 100}%`, background: OPTION_COLORS[option] }} />
        ) : null,
      )}
    </div>
  );
}

/** What the acting range does here as a whole, and how well the strategy covers it. */
function SpotSummary({ spot, game }: { spot: SpotSection; game: GameSection }) {
  const valid = 1326 - spot.invalid;
  return (
    <section className="border-b px-4 py-3" style={{ borderColor: 'var(--color-rule)' }}>
      <Heading aside={`${game.actor.name} to act`}>spot</Heading>
      <Row label="records">{spot.records}</Row>
      <Row label="range">
        {spot.rangeCombos} combos · weight {spot.rangeWeight.toFixed(1)}
      </Row>
      <Row label="range ev">{formatEv(spot.rangeEv)} chips</Row>
      <Row label="coverage">
        {spot.found} of {valid} found{spot.missing ? ` · ${spot.missing} missing` : ''}
      </Row>
      <div className="mt-2 mb-1">
        <MixBar mix={spot.rangeMix} />
      </div>
      <div className="flex flex-wrap gap-x-3 text-[11px]">
        {game.options.map((option) =>
          option.legal ? (
            <span key={option.option} style={{ color: OPTION_COLORS[option.option] }}>
              {option.name}
              {option.size ? ` ${option.size}` : ''} {formatShare(spot.rangeMix[option.option])}
            </span>
          ) : null,
        )}
      </div>
      {spot.replayDrops.combos > 0 ? (
        <div className="mt-2 text-[11px]" style={{ color: 'var(--color-act-big)' }}>
          {spot.replayDrops.groups} cluster{spot.replayDrops.groups === 1 ? '' : 's'} on the line had no
          record; their {spot.replayDrops.combos} combos were dropped from the range
        </div>
      ) : null}
      {spot.emptySeats.length > 0 ? (
        <div className="mt-2 text-[11px]" style={{ color: 'var(--color-act-big)' }}>
          empty range: {spot.emptySeats.map((seat) => game.seats[seat]?.name ?? seat).join(', ')} — training
          would drop this hand
        </div>
      ) : null}
    </section>
  );
}

/** Everything known about the combination under the cursor. */
function HoverReadout({ hovered, spot, game }: {
  hovered: number | null;
  spot: SpotSection;
  game: GameSection;
}) {
  if (hovered === null) {
    return (
      <section className="border-b px-4 py-3 text-[11px]" style={{ borderColor: 'var(--color-rule)', color: 'var(--color-ash)' }}>
        hover a combination for its strategy
      </section>
    );
  }
  const [first, second] = COMBO_CARDS[hovered];
  const name = comboName(first, second);
  const row = spot.combos[hovered];

  return (
    <section className="border-b px-4 py-3" style={{ borderColor: 'var(--color-rule)' }}>
      <Heading aside={row ? `cluster ${row.cluster}` : undefined}>
        <span className="text-[12px] tracking-normal normal-case" style={{ color: 'var(--color-chalk)' }}>
          {name}
        </span>
      </Heading>
      {!row ? (
        <Row label="status">uses a board card</Row>
      ) : (
        <>
          <Row label="reach">{row.reach.toFixed(4)}</Row>
          {row.found ? (
            <>
              <Row label="ev">{formatEv(row.ev ?? 0)} chips</Row>
              <Row label="cluster">{row.clusterCombos} combos</Row>
              <div className="mt-1.5 grid grid-cols-[1fr_4rem_4.5rem] gap-x-2 text-[11px]">
                <span style={{ color: 'var(--color-ash)' }}>option</span>
                <span className="text-right" style={{ color: 'var(--color-ash)' }}>freq</span>
                <span className="text-right" style={{ color: 'var(--color-ash)' }}>ev</span>
                {game.options.map((option) =>
                  option.legal ? (
                    <div key={option.option} className="contents">
                      <span style={{ color: OPTION_COLORS[option.option] }}>
                        {option.name}
                        {option.size ? ` ${option.size}` : ''}
                      </span>
                      <span className="text-right">{formatShare(row.frequencies?.[option.option] ?? 0)}</span>
                      <span className="text-right">{formatEv(row.evOptions?.[option.option] ?? 0)}</span>
                    </div>
                  ) : null,
                )}
              </div>
            </>
          ) : (
            <Row label="status">no record for this cluster</Row>
          )}
          <Row label="key" dim>
            {row.key}
          </Row>
        </>
      )}
    </section>
  );
}

/** Muted tones that keep adjacent fields apart without competing with the actions. */
const FIELD_TONES = ['#e8edf2', '#9fb3c8', '#f2c46d', '#7fd1b9', '#c7a0f0', '#f29b7a', '#8fc0f5'];

function fieldTone(index: number): string {
  return FIELD_TONES[index % FIELD_TONES.length];
}

/** The 58 shared bits, most significant first, coloured by field. */
function BitStrip({ fields }: { fields: KeyField[] }) {
  const ordered = fields.map((field, index) => ({ field, index })).sort((a, b) => b.field.shift - a.field.shift);
  return (
    <div className="flex flex-wrap gap-x-[3px] gap-y-1 text-[10px] leading-none break-all">
      {ordered.map(({ field, index }) => (
        <span
          key={field.name}
          title={`${field.name} · bits ${field.shift}-${field.shift + field.width - 1}`}
          style={{ color: field.shared ? fieldTone(index) : 'var(--color-ash)' }}
        >
          {field.shared ? field.bits : '·'.repeat(field.width)}
        </span>
      ))}
    </div>
  );
}

/** The key as the solver assembles it, field by field, with what each value means. */
function KeyPanel({ keySection, game }: { keySection: KeySection; game: GameSection }) {
  const villan = game.seats[keySection.villanSeat]?.name ?? String(keySection.villanSeat);
  return (
    <section className="px-4 py-3">
      <Heading aside={keySection.sharedKey}>key</Heading>
      <div className="mb-1 text-[11px]" style={{ color: 'var(--color-ash)' }}>
        hero {game.seats[keySection.heroSeat]?.name} · villain {villan} · spr {keySection.spr.toFixed(2)} · cpr{' '}
        {keySection.cpr.toFixed(2)}
      </div>
      <div className="mb-2">
        <BitStrip fields={keySection.fields} />
        <div className="mt-1 text-[10px]" style={{ color: 'var(--color-ash)' }}>
          bits 57 → 0 · bits 58–63 hold each combo's hand cluster
        </div>
      </div>
      <div className="flex flex-col gap-2">
        {keySection.fields.map((field, index) => (
          <div key={field.name} className="text-[11px]">
            <div className="flex items-baseline justify-between gap-2">
              <span style={{ color: fieldTone(index) }}>{field.label}</span>
              <span style={{ color: 'var(--color-ash)' }}>
                {field.width === 1 ? field.shift : `${field.shift + field.width - 1}–${field.shift}`}
              </span>
            </div>
            <div className="flex items-baseline justify-between gap-2">
              <span style={{ color: field.zeroed ? 'var(--color-ash)' : 'var(--color-chalk)' }}>
                {field.shared ? `${field.bits} = ${field.value}` : 'per combo'}
              </span>
              <span className="text-right" style={{ color: field.zeroed ? 'var(--color-ash)' : 'var(--color-chalk)' }}>
                {field.meaning}
              </span>
            </div>
            {field.parts.length > 0 ? (
              <div className="mt-0.5 border-l pl-2" style={{ borderColor: 'var(--color-rule)' }}>
                {field.parts.map((part) => (
                  <div key={part.name} className="flex items-baseline justify-between gap-2">
                    <span style={{ color: 'var(--color-ash)' }}>
                      {part.name} = {part.value}
                    </span>
                    <span className="text-right" style={{ color: part.zeroed ? 'var(--color-ash)' : undefined }}>
                      {part.meaning}
                    </span>
                  </div>
                ))}
              </div>
            ) : null}
          </div>
        ))}
      </div>
    </section>
  );
}

export function SpotColumn({
  game,
  spot,
  keySection,
  hovered,
}: {
  game: GameSection | null;
  spot: SpotSection | null;
  keySection: KeySection | null;
  hovered: number | null;
}) {
  return (
    <aside
      className="flex w-[360px] shrink-0 flex-col overflow-y-auto border-l"
      style={{ borderColor: 'var(--color-rule)', background: 'var(--color-slate)' }}
    >
      {game && spot ? (
        <>
          <SpotSummary spot={spot} game={game} />
          <HoverReadout hovered={hovered} spot={spot} game={game} />
        </>
      ) : null}
      {game && keySection ? <KeyPanel keySection={keySection} game={game} /> : null}
    </aside>
  );
}
