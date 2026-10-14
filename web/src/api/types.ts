// Mirrors the POST /strategy contract. Sections are optional only because "include"
// can turn them off; the client always asks for all three.

export const OPTION_COUNT = 5;
export const COMBO_COUNT = 1326;

/** Option indices, in the order the engine uses them. */
export const FOLD = 0;
export const CALL = 1;
export const SMALL = 2;
export const BIG = 3;
export const SHOVE = 4;

export type OptionIndex = 0 | 1 | 2 | 3 | 4;

/** The four settings a strategy file was trained at; the spec names the file. */
export interface Config {
  players: number;
  chips: number;
  metasBasic: boolean;
  memoryBasic: boolean;
}

export interface ConfigWithSpec extends Config {
  spec: string;
}

export interface MetaResponse {
  defaults: ConfigWithSpec;
  limits: { players: [number, number]; chipsMax: number; chipsEven: boolean };
  available: ConfigWithSpec[];
  blinds: [number, number];
  /** The inverse temperature training ran at, ln(2^8). */
  invTemp: number;
  /** What a request may ask for: [ln(2^4), ln(2^64)]. */
  invTempRange: [number, number];
  rangeFloor: number;
  comboCount: number;
  options: string[];
  /** Seat names by player count, keyed "3".."7". */
  seats: Record<string, string[]>;
  evUnits: string;
  replay: string;
}

export interface Seat {
  seat: number;
  name: string;
  stack: number;
  /** Chips in front on this street. */
  bet: number;
  inHand: boolean;
  allIn: boolean;
  acting: boolean;
}

export interface Actor {
  seat: number;
  name: string;
  stack: number;
  bet: number;
  toCall: number;
  potAfterCall: number;
  spr: number;
  cpr: number;
}

export interface OptionView {
  option: number;
  /** fold, check, call, bet, raise or all in - as the table would say it. */
  name: string;
  /** "0.5x pot" or "1.5x pot" for the two sized raises, else "". */
  size: string;
  legal: boolean;
  /** Total in front after the option; null for fold and illegal options. */
  betTo: number | null;
  adds: number | null;
  /** Why getOptions() turned it off; "" when legal. */
  reason: string;
}

export type HistoryEntry =
  | {
      kind: 'post' | 'action';
      seat: number;
      name: string;
      option: number;
      round: number;
      betTo: number;
      adds: number;
      stack: number;
    }
  | { kind: 'street'; round: number; street: string; board: string };

export interface GameSection {
  config: ConfigWithSpec;
  round: 0 | 1 | 2 | 3;
  street: 'preflop' | 'flop' | 'turn' | 'river';
  board: string;
  /** Every chip committed, this street's bets included. */
  pot: number;
  heads: number;
  blinds: string;
  seats: Seat[];
  actor: Actor;
  legal: boolean[];
  options: OptionView[];
  history: HistoryEntry[];
}

export interface KeyPart {
  name: string;
  value: number;
  meaning: string;
  zeroed: boolean;
}

export interface KeyField {
  name: string;
  label: string;
  shift: number;
  width: number;
  value: number;
  /** The field's bits, most significant first. */
  bits: string;
  meaning: string;
  /** memoryBasic forces this field to 0. */
  zeroed: boolean;
  /** False for the preflop board field, which holds each combo's own 169 class. */
  shared: boolean;
  parts: KeyPart[];
}

export interface KeySection {
  /** actionState | boardField << 42 - every bit but the hand cluster. Hex. */
  sharedKey: string;
  actionState: string;
  heroSeat: number;
  villanSeat: number;
  spr: number;
  cpr: number;
  sharedBits: number;
  fields: KeyField[];
}

/**
 * null when the combination uses a board card. found: false when its node has no
 * record, in which case only reach, cluster, key and clusterCombos are present.
 */
export interface SpotRow {
  combo: string;
  /** The acting seat's reach: its own policy multiplied along the line. */
  reach: number;
  /** The 169 class preflop, the hand cluster postflop. */
  cluster: number;
  key: string;
  found: boolean;
  clusterCombos: number;
  /** Options 1..4, pot normalised against folding. null where illegal. */
  weights?: (number | null)[];
  /** Five entries, fold first, summing to 1 over the legal options. */
  frequencies?: number[];
  /** Each option's value in chips relative to folding. null where illegal. */
  evOptions?: (number | null)[];
  /** The policy-weighted EV in chips relative to folding. */
  ev?: number;
  /** The record's visits, shared by every combo in its cluster. */
  visits?: number;
}

export type SpotCombo = SpotRow | null;

export interface SpotSection {
  legal: boolean[];
  /** The inverse temperature the ranges were replayed and the policy taken at. */
  invTemp: number;
  potAfterCall: number;
  /** Visits summed over the distinct records the acting range reaches. */
  totalVisits: number;
  records: number;
  found: number;
  missing: number;
  invalid: number;
  rangeCombos: number;
  rangeWeight: number;
  rangeFoundWeight: number;
  rangeMix: number[];
  rangeEv: number;
  emptySeats: number[];
  replayDrops: { decisions: number; groups: number; combos: number };
  combos: SpotCombo[];
}

export interface StrategyResponse {
  game?: GameSection;
  key?: KeySection;
  spot?: SpotSection;
}

export interface StrategyRequest {
  config: Config;
  invTemp: number;
  actions: number[];
  board: string;
}

/** True when the row carries a usable strategy. */
export function isHit(row: SpotCombo): row is SpotRow & Required<Pick<SpotRow, 'frequencies'>> {
  return row !== null && row.found && row.frequencies !== undefined;
}
