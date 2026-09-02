// Mirrors the POST /strategy contract. Every section is optional because the
// server computes nothing it was not asked for - an absent section is "not
// requested", never "empty".

export const OPTION_COUNT = 5;
export const COMBO_COUNT = 1326;

/** Option indices, in the order the engine uses them. */
export const FOLD = 0;
export const CALL = 1;
export const SMALL = 2;
export const BIG = 3;
export const SHOVE = 4;

export type OptionIndex = 0 | 1 | 2 | 3 | 4;

export interface MetaResponse {
  spec: string;
  playerCount: number;
  startingChips: number;
  blinds: [number, number];
  defaultInvTemp: number;
  comboCount: number;
  options: string[];
  seats: string[];
  records: number;
}

export interface NodeSection {
  spec: string;
  round: 0 | 1 | 2 | 3;
  street: 'preflop' | 'flop' | 'turn' | 'river';
  seat: number;
  heads: number;
  board: string;
  /** Recomputed by getOptions(), never read from the strategy file. */
  legal: boolean[];
  options: string[];
  invTemp: number;
  weightUnits: string;
}

export interface LastAction {
  seat: number;
  option: number;
  /** Stack after the action. Authoritative - derive bet sizes from this. */
  chips: number;
  /** 0 whenever the action closed the street, because newRound() zeroes bets. */
  bet: number;
  inHand: boolean;
}

export interface Actor {
  seat: number;
  chips: number;
  bet: number;
  toCall: number;
  potAfterCall: number;
  spr: number;
  cpr: number;
  legal: boolean[];
}

export interface TransitionSection {
  pot: number;
  lastAction: LastAction | null;
  actor: Actor;
}

export interface KeyComponents {
  roundHeadsBasic: number;
  headsBasic: number;
  actionPositionBucket: number;
  sprCprBucket: number;
  sprCprKind: 'spr' | 'cpr';
  boardField: number | null;
  boardClass: number | null;
  raiseHistory: number[];
  playerLastActions: number[];
  villanLastActions: number[];
  playerPositionBasic: number;
  villanPositionBasic: number;
  villanPosition: number;
  raiseSizeOverZero: number;
  raiseStrength: number;
  hiddenRaiseSizeOverZero: number;
  hiddenRaiseStrength: number;
  raisesFaced: number;
  relativePosition: number;
  playerCallsOverOne: boolean;
  calls: number;
}

/**
 * A combo row has four shapes and they are not interchangeable:
 *   null                            the combination uses a board card
 *   { found: false, metaFound: false }  meta miss - no (boardClass, handClass) entry
 *   { found: false }                table miss - key absent from the strategy table
 *   { found: true, ... }            hit
 * `frequencies` and `visits` are only meaningful on the last of those.
 */
export interface ComboRow {
  combo: string;
  found?: boolean;
  metaFound?: boolean;
  /** Five entries, fold first, summing to 1 over the legal options. */
  frequencies?: number[];
  /** Four entries for options 1..4. null where illegal - never treat as 0. */
  weights?: (number | null)[];
  /** Per-holding estimate: nodeVisits / metaCombos. */
  visits?: number;
  nodeVisits?: number;
  metaCombos?: number;
  key?: string;
  handMeta?: number;
}

export type Combo = ComboRow | null;

export interface Counts {
  found: number;
  missing: number;
  invalid: number;
  metaMissing: number;
}

export interface StrategyResponse {
  node?: NodeSection;
  transition?: TransitionSection;
  keyComponents?: KeyComponents;
  combos?: Combo[];
  counts?: Counts;
}

export interface StrategyRequest {
  actions: number[];
  board: string;
  invTemp?: number;
  include: Record<string, unknown>;
}

/** True when the row carries a usable strategy, i.e. it is a hit. */
export function isHit(row: Combo): row is ComboRow {
  return row !== null && row.found === true;
}
