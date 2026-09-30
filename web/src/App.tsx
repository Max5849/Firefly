import { useEffect, useState } from 'react';
import { RangeGrid } from './grid/RangeGrid';
import { useHand } from './hand/useHand';
import { ActionColumn } from './ui/ActionColumn';
import { Header } from './ui/Header';

export default function App() {
  const hand = useHand();
  const [hovered, setHovered] = useState<number | null>(null);
  const { handleKey } = hand;

  // There is no control surface but the keyboard, so the listener is on the window.
  useEffect(() => {
    function onKeyDown(event: KeyboardEvent) {
      if (event.metaKey || event.ctrlKey || event.altKey) return;
      const target = event.target as HTMLElement | null;
      if (target?.tagName === 'INPUT' || target?.tagName === 'TEXTAREA') return;
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

  return (
    <div className="flex h-full flex-col">
      <Header />
      <div className="flex min-h-0 flex-1">
        <ActionColumn {...hand} hovered={hovered} />
        <RangeGrid response={response} black={hand.black} onHover={setHovered} />
      </div>
    </div>
  );
}
