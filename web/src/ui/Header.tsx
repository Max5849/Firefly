import type { Config, MetaResponse } from '../api/types';
import { ConfigBar } from './ConfigBar';

/** The wordmark is the one non-data element on the page, and the one non-mono type. */
export function Header({
  meta,
  config,
  onConfig,
}: {
  meta: MetaResponse | null;
  config: Config | null;
  onConfig: (config: Config) => void;
}) {
  return (
    <header
      className="flex h-14 shrink-0 items-center justify-between gap-6 border-b px-5"
      style={{ borderColor: 'var(--color-rule)', background: 'var(--color-slate)' }}
    >
      <h1
        className="text-[15px] font-extrabold tracking-[0.22em] uppercase"
        style={{ fontFamily: 'var(--font-display)', color: 'var(--color-chalk)' }}
      >
        Tashtego<span style={{ color: 'var(--color-ash)' }}>GTO</span>
      </h1>
      <ConfigBar meta={meta} config={config} onChange={onConfig} />
    </header>
  );
}
