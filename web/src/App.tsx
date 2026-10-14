import { useEffect, useState } from 'react';
import { RangeGrid } from './grid/RangeGrid';
import { useHand } from './hand/useHand';
import { ActionColumn } from './ui/ActionColumn';
import { Header } from './ui/Header';
import { SpotColumn } from './ui/SpotColumn';

export default function App() {
  const hand = useHand();
  const [hovered, setHovered] = useState<number | null>(null);
  const { handleKey } = hand;

  // The hand is played from the keyboard, so the listener is on the window.
  useEffect(() => {
    function onKeyDown(event: KeyboardEvent) {
      if (event.metaKey || event.ctrlKey || event.altKey) return;
      const target = event.target as HTMLElement | null;
      if (target?.tagName === 'INPUT' || target?.tagName === 'TEXTAREA' || target?.tagName === 'SELECT') return;
      // Space and Enter would otherwise re-activate whichever button last held
      // focus, on a page whose every real control is a key.
      if (target?.tagName === 'BUTTON' && (event.key === ' ' || event.key === 'Enter')) {
        target.blur();
        event.preventDefault();
        return;
      }
      if (event.key === 'Backspace') event.preventDefault();
      handleKey(event.key);
    }
    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [handleKey]);

  // The grid empties when the hand ends, rather than freezing on a stale range.
  const response = hand.mode === 'ended' ? null : (hand.frame?.response ?? null);
  const missing = hand.missing;

  return (
    <div className="flex h-full flex-col">
      <Header meta={hand.meta} config={hand.config} onConfig={hand.setConfig} />
      {missing ? (
        <div
          className="shrink-0 border-b px-5 py-2 text-[12px]"
          style={{ borderColor: 'var(--color-rule)', color: 'var(--color-act-big)' }}
        >
          {missing.message}
          {missing.available.length > 0
            ? ` — trained: ${missing.available.map((entry) => entry.spec).join(', ')}`
            : ' — no strategies are trained yet'}
        </div>
      ) : null}
      <div className="flex min-h-0 flex-1">
        <ActionColumn {...hand} />
        <RangeGrid response={response} black={hand.black} onHover={setHovered} />
        <SpotColumn
          game={response?.game ?? null}
          spot={response?.spot ?? null}
          keySection={response?.key ?? null}
          hovered={hovered}
          black={hand.black}
        />
      </div>
    </div>
  );
}
