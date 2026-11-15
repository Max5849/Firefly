# server — the strategy API

Serves the strategies solver/singleHand trains over HTTP. Given a config, an action
sequence and a board, it returns three things at the node that sequence reaches:

- **`game`**: the table as a player sees it. Seats, stacks, bets, pot, board, whose
  turn it is, what they may do and for how much, and the hand so far.
- **`key`**: the acting seat's state key, decoded field by field from its own bits.
- **`spot`**: for all 1326 hole card combinations, the actions, the acting seat's
  range (reach), the EV in chips, and each option's return variance.

## Why it replays

The 64-bit state key is the accumulated residue of `Engine::play()`. Most of its
fields (`raiseHistoryCompressed`, the story digits, `villanPosition`,
`relativePosition`, `raisesFaced`, the street's `calls` and `hiddenCalls`, the raise
descriptors, both heads buckets) exist nowhere but in that accumulation. So the API
replays the action list through an engine rather than reconstructing a node.

That engine is `TableEngine<P>` in [engine/tableEngine.h](engine/tableEngine.h). It
holds the game rules of [solver/singleHand/engine.h](../solver/singleHand/engine.h)
(deal, play, autoFold, setActionState, getOptions, beginNewRound, the meta state) for
any table size from 3 to 7 and any stack and ante. It leaves out hole cards, showdown
and rewards. The trainer's engine fixes `PLAYER_COUNT`, `CHIP_COUNT` and `ANTE` at
compile time and is left untouched.

Because the rules are a copy, `build/keyEquivalence` plays `TableEngine<PLAYER_COUNT>`
in lockstep with the trainer's `Engine` and fails on any difference. **A rule or key
change in solver/singleHand/engine.h has to be copied into tableEngine.h, and a key
layout change into keyLayout.h.** [engine/tables.h](engine/tables.h) loads the lookup
tables through `Engine`'s own loaders, minus `handScores.bin`, one metas stage at a
time on first use.

The trainer deals real hole cards and keeps no ranges, so the server builds them: a
combo's reach is the product of its seat's own policy along the line. At every past
decision the replay looks up the acting seat's nodes in the strategy and multiplies
its range by each node's probability for the option played
(`TableEngine::applyPolicy`). There is no floor. A node with **no record drops the
combos that reach it**, and the response counts those drops (`spot.replayDrops`).

**Weights are pot-relative.** The trainer stores each option's return as a share of
the pot after calling, relative to folding. Every policy here is `P(j) ∝ exp(invTemp ×
w_j)` over those shares, both at each past decision and at the spot, so `invTemp` is per
pot. The default is ln(2^20), singleHand's `INV_TEMP`, the policy self-play draws from.
EVs are reported in chips: `weight × potAfterCall`.

**Forced folds.** A seat that has already called twice on a street folds to any
further bet by itself (`Engine::autoFold`): no key, no decision. The replay plays these
folds as the trainer does. They appear in `game.history` as `autoFold` entries, and
they take **no slot in `actions`**, which holds decisions only.

## Building

From the repository root, always:

```
clang++ -std=c++2b -O2 -I. -pthread -o build/server server/server.cpp -lz
clang++ -std=c++2b -O2 -I. -o build/indexBuilder    server/indexBuilder.cpp
clang++ -std=c++2b -O2 -I. -o build/keyEquivalence  server/tests/keyEquivalence.cpp
clang++ -std=c++2b -O2 -I. -o build/storeProbe      server/tests/storeProbe.cpp
```

On the Oracle VM substitute `g++` (13 or newer) and install `zlib1g-dev`.

## Running

```
./build/server --host 127.0.0.1 --port 8080 --warm
```

Must be run from the repository root, because the handAnalysis tables and
`solver/strategies/` are resolved through relative paths. `--threads` sizes the
worker pool. `--warm` loads the default metas stage's tables at startup and faults
each strategy's mappings in when it is first opened.

Strategy files are opened **on first request** for their spec, and stay open after
that. If `strategy_<spec>.idx` is missing or does not match the `.bin` file, the
server rebuilds it beside the strategy file: about 25 seconds and a 2 GB index at
126M records. `build/indexBuilder` does the same ahead of time:

```
./build/indexBuilder                                     # the spec config.h names
./build/indexBuilder --players 7 --chips 400 --ante 2 --metas 1 --memory 0 --tag A
```

### Strategy files and key layouts

A strategy file is `strategy_<spec>.bin`, with
`spec = 100000·players + 100·chips + 100·ante + 10·metasStage + memoryStage`, then the
tag letter: 7 players, 400 chips, ante 2, metas stage B (1), memory stage A (0), tag A
is `740210A`. The chips and ante terms overlap unless the stack is a multiple of 10,
so the server only accepts such stacks, from 10 to 990.

Nothing in a file says which key layout it was written with, so when the registry
opens one it looks up the config's opening node (seat `3 % players`, nothing played)
in both layouts and keeps the one that finds it (`detectKeyFormat` in replay.h):

| `keyFormat` | Layout |
| --- | --- |
| `current` | action state in bits 0–42, board field 43–57, 6-bit hand cluster 58–63 (solver/singleHand/config.h today) |
| `bit44` | the layout before 2026-11-15: the same action state, the acting seat's `calls > 1` at bit 43, board field 44–58, 5-bit cluster 59–63 |

Every lookup takes a current-layout key and translates it. The rules are otherwise
the same, so a `bit44` file serves exactly: `strategy_740210A.bin` holds the key at
100% of on-policy preflop and flop decisions. The startup log, `/health` and
`spot.keyFormat` name each file's layout. Rows' `key`s are always the current layout.

## Endpoints

### `POST /strategy`

```json
{
  "config": { "players": 7, "chips": 400, "ante": 2, "metasStage": 1, "memoryStage": 0, "tag": "A" },
  "actions": [1, 0, 0, 0, 0, 1, 1],
  "board": "As7h2d",
  "invTemp": 13.862943611198906,
  "include": { "game": true, "key": true, "spot": true }
}
```

| Field | Meaning |
| --- | --- |
| `config` | Which strategy to read. Each field is optional and defaults to solver/singleHand/config.h. `players` 3–7. `chips` a multiple of 10 from 10 to 990. `ante` 0–9, a dead ante in the pot from the deal. `metasStage` and `memoryStage` 0–2 (0 = A, the full detail). `tag` one letter. The old `metasBasic` / `memoryBasic` booleans are rejected with a pointer to the stages (`true` was stage 1). |
| `actions` | Decisions in order: 0 fold, 1 call/check, 2 raise to half pot, 3 raise to one and a half pot, 4 all in. Forced folds are not listed. Empty means the first preflop decision (seat `3 % players`). |
| `board` | `"As7h2d"`, `"As 7h 2d"` or `"As,7h,2d"`. It must hold exactly the cards for the street the actions reach: 0 or 3. |
| `invTemp` | Optional inverse temperature **per pot**, from ln(2^5) ≈ 3.466 to ln(2^80) ≈ 55.45. Defaults to ln(2^20), singleHand's `INV_TEMP`. It sets **both** the policy each past decision narrows its range with and the policy shown at the spot, so ranges, frequencies, EV and the range mix all belong to one temperature. Echoed as `spot.invTemp`. |
| `include` | Optional. Leaving it out means all three sections. When present, only the sections set to `true` are returned. |

An unknown field or section is a 400 that names it.

#### `game`

```json
{ "config": {"players": 7, "chips": 400, "ante": 2, "metasStage": 1, "memoryStage": 0,
             "tag": "A", "spec": "740210A"},
  "round": 1, "street": "flop", "board": "As7h2d", "pot": 8, "heads": 3,
  "blinds": "1/2", "ante": 2,
  "seats": [{"seat": 0, "name": "BTN", "stack": 400, "bet": 0, "inHand": false,
             "allIn": false, "acting": false}, ...],
  "actor": {"seat": 1, "name": "SB", "stack": 398, "bet": 0, "toCall": 0,
            "potAfterCall": 8, "spr": 49.75, "cpr": 0},
  "legal": [false, true, true, true, false],
  "options": [{"option": 2, "name": "bet", "size": "0.5x pot", "legal": true,
               "betTo": 4, "adds": 4, "reason": ""}, ...],
  "history": [{"kind": "post", "seat": 1, "name": "small blind", "betTo": 1, ...},
              {"kind": "action", "seat": 3, "name": "call", "option": 1, "round": 0,
               "betTo": 2, "adds": 2, "stack": 398},
              {"kind": "autoFold", "seat": 3, "name": "fold", "option": 0, ...},
              {"kind": "street", "round": 1, "street": "flop", "board": "As7h2d"}] }
```

- `pot` counts every chip committed, the ante and this street's bets included.
- `seats[].bet` is the amount in front on the current street.
- Option `name` is what the table would say: check or call, and bet or raise.
- `betTo` comes from `TableEngine::betTo`, which repeats `play()`'s float
  operations; the test asserts every raise lands on it.
- Illegal options carry the `reason` `getOptions()` turned them off for.
- `history` holds `post`, `action`, `autoFold` and `street` entries. The k-th
  `action` entry is decision k; an `autoFold` follows the action that left the
  seat facing a bet.
- Seat names follow the player count: BTN, SB, BB, then the last `P − 3` of UTG, LJ,
  HJ, CO.

#### `key`

The shared key is `actionState | boardField << 43`. It covers bits 0–57
(`sharedBits`): every bit except the hand cluster (58–63), which differs per combo and
is given in each `spot` row. Fields, in order:

| Field | Bits | Holds |
| --- | --- | --- |
| `headsBasic` | 0–1 | players in the hand: 2, 3, 4–5, 6–7 |
| `preflopHeadsBasic` | 2–3 | the same bucket, frozen when preflop ends |
| `sprCpr` | 4–7 | SPR 0–5 (cuts 0.5, 1.5, 3.5, 7.5, 15.5), or with no chips behind CPR 6–9 (cuts 0.5, 1.5, 3.5) |
| `raiseHistory` | 8–15 | colex rank of the four per-street raise counts (parts: the counts) |
| `raiseSizes` | 16–19 | per street, whether its last raise was 1.5x pot or all in |
| `story` | 20–30 | one base-3 digit per street, then hero and villain position (base 5); positions are zeroed after a limped preflop |
| `lastRaise` | 31–35 | only on a raised street: what the raiser faced, seats behind it, calls before it |
| `hiddenRaiseSize` | 36 | only from the street's second raise: whether the raise before was big |
| `hero` | 37–41 | seats behind hero, raises hero faces, calls since the pot opened or was raised |
| `relativePosition` | 42 | whether hero acts after the villain postflop |
| `boardField` | 43–57 | board class + 169 postflop; preflop each combo's 169 class (stage C: its cluster), so `"shared": false` |

For each field the section lists `shift`, `width`, `bits` (most significant first),
`value`, `meaning`, `zeroed` (the key holds the field at 0 at this node, whatever the
hand did) and `parts`. Every memory stage builds the same action state today, so the
layout does not depend on it. A past street where hero and villain both raised last
writes a story digit of 3, which carries into the digit above; the raw `value` is
still exact.

The section also carries `heroSeat`, `villanSeat` (needed to read the villain
digits, but not stored in the key), the exact `spr` and `cpr`, and `actionStateBits`.

#### `spot`

```json
{ "keyFormat": "bit44", "legal": [...], "invTemp": 13.86, "potAfterCall": 8,
  "records": 18, "found": 1176, "missing": 0, "invalid": 150,
  "rangeCombos": 1176, "rangeWeight": 615.26, "rangeFoundWeight": 615.26,
  "rangeMix": [0, 0.757, 0.221, 0.022, 0], "rangeEv": 0.31,
  "emptySeats": [], "replayDrops": {"decisions": 0, "groups": 0, "combos": 0},
  "combos": [null, {"combo": "KcQd", "reach": 0.33, "cluster": 8, "key": "0x...",
                    "found": true, "clusterCombos": 41,
                    "weights": [-0.627, -1.195, -1.737, null],
                    "variances": [2.1, 2.41, 6.87, null],
                    "frequencies": [0, 0.957, 0.041, 0.002, 0],
                    "evOptions": [null, -5.02, -9.56, -13.9, null], "ev": -5.2},
         ...] }
```

- **Rows** are in hole-pair index order, `i * (101 - i) / 2 + j - 1` for cards
  `i < j`. A row is `null` when the combo uses a board card. On a row with
  `"found": false`, the node has no record, so only `reach`, `cluster`, `key` and
  `clusterCombos` are present.
- **`cluster`** is the combo's group: the 169 class preflop (stage C: its preflop
  cluster), the hand cluster postflop. Combos in a group share a key and a record.
- **`reach`** is the acting seat's: the product of its own policy along the line. It
  is 0 for a combo the line has ruled out, which still carries its strategy.
- **`weights`** are the record's four values for options 1–4: returns as a share of
  the pot after calling, relative to folding, which is exactly 0, stored to 1/1024 of a
  pot and clamped to [-32, 31.999]. An illegal option is `null`.
- **`variances`** are the record's variance of each of those returns, in pots
  squared, decoded from its 16-bit codes and clamped to [1/16, 90000]. The code scale
  is fixed, so it does not depend on the `VARIANCE_MIN` a run trained with. An illegal
  option is `null`.
- **`frequencies`** is the softmax of `invTemp × weights` over the legal options,
  fold first.
- **`evOptions`** gives each option's value in **chips relative to folding**:
  `weight × potAfterCall`. `ev` is the policy-weighted mean.
- **`clusterCombos`** is how many valid combos share the row's record. Files carry
  no visit counts.
- **`rangeMix`** and **`rangeEv`** are reach-weighted over the combos that have a
  record.
- **`emptySeats`** lists seats whose range the line has emptied: every combo they
  could hold passed a node with no record.

The weight and variance codecs are solver/singleHand/strategyTable.h's own
(`decodeWeight`, `decodeVariance`), included rather than copied.

#### Errors

Every error body has a machine-readable `code` and an `error` message:

| Status | `code` | Extra fields | Cause |
| --- | --- | --- | --- |
| 400 | `illegal` | `reason`, `actionIndex`, `legal`, `options` | the tree forbids the action here |
| 400 | `needsBoard` | `street`, `cards`, `actionIndex` | the action closed a street and the board is short |
| 400 | `handEnded` | `endedBy` (`fold` / `showdown`), `actionIndex` | nothing is left to decide |
| 400 | `boardMismatch` | | too many board cards for the street reached |
| 400 | `badAction`, `badBoard`, `badConfig`, `badRequest` | | malformed input |
| 404 | `strategyMissing` | `spec`, `path`, `config`, `available` | no `strategy_<spec>.bin` for that config |
| 500 | `tablesMissing` | `metasStage` | the stage's hand-analysis tables cannot be loaded |
| 500 | `strategyUnreadable`, `internal` | | a corrupt file, or an unexpected throw |

### `GET /meta`, `GET /health`

`/meta` returns:
- the default config (solver/singleHand/config.h's)
- the limits: players, chips and their step, ante, the stage counts
- `metasStagesLoaded`: which stages' tables are in memory so far
- the `available` strategies, decoded from `solver/strategies/strategy_*.bin`
- seat names per player count
- the default `invTemp` and `invTempRange` (per pot), the blinds, the option names,
  the card encoding, and notes on the replay and the streets played

`/health` lists the specs opened so far, their record counts and key layouts.

## What the tree will not accept

`actions` are checked against `getOptions()` before each one is played:

- **Raises are only** half pot, one and a half pot, or all in. Which of these are
  offered depends on the SPR at that node: half pot needs SPR ≥ 1.5, one and a half
  pot SPR ≥ 3.5, and all in is offered only below SPR 7.5.
- **At most six raises** per hand, and the sixth can only be all in.
- No flat call when facing three or more raises since you last acted.
- Limping is allowed: the first preflop call opens the pot.

**Hands are preflop and flop only.** `beginNewRound()` ends the betting after the
flop, and after a raised preflop with more than three players left, so those lines
answer `handEnded` / `showdown`. An off-tree action is not projected onto the nearest
legal one.

## Tests

```
./build/keyEquivalence 4000
```

The test runs in two parts.

1. **Lockstep.** The trainer's `Engine` (singleHand, `PLAYER_COUNT` seats,
   `CHIP_COUNT`, `ANTE`) and `TableEngine<PLAYER_COUNT>` play the same hands the way
   `Explorer::traverse` does. They must agree on `autoFold`, every memory stage's
   action state, the options, every seat's chips, bets, villain and calls, what
   `play()` and `beginNewRound()` return, and every built metas stage's meta for each
   seat's own two cards. The trainer's table statics are copied from the server's, so
   neither `handScores.bin` nor an unbuilt stage is needed. When the default strategy
   file exists, play follows its policy at `INV_TEMP` and the test reports the share
   of decisions whose key the file holds.
2. **Every table size from 3 to 7.** `TableEngine` self-plays under random metas and
   memory stages, stacks and antes, with calls weighted up so the walk reaches the
   flop. At every decision the test asserts that:
   - `Replay`, given only the action list and the revealed board, reaches the same
     action state, acting seat, legal set and per-combo meta state, auto folds
     included;
   - every `key` field equals the engine quantity it encodes, and the fields repack
     into the shared key;
   - every illegal option has a reason and no legal one does;
   - every raise lands on its `betTo`.

At 4000 hands: 40k lockstep decisions, 183k replay nodes, 3k auto folds, 0 failures,
and `strategy_740210A.bin` (`bit44`) holds 100% of on-policy preflop and flop keys.
A metas stage whose tables are not built is skipped, and the test names it.

`build/storeProbe [--spec 740210A] <hex keys...>` prints what the mmap path returns
for a key, taken exactly as the file stores it.

## On the VM

**Step-by-step instructions are in [DEPLOY.md](DEPLOY.md).** Copy these from the
repository root:

- `solver/handAnalysis/boardClasses<L>.bin` and `handMetas<L>.bin` for each metas
  stage served (`A`, `B`, `C`), plus `preflopMetas.bin` for stage C
- every `solver/strategies/strategy_<spec>.bin` to serve (8 + 24 bytes per record:
  uint64 key, int16 weights[4] in 1/1024 pot steps, uint64 packed variance codes —
  singleHand's format; any other record size is rejected), and its `.idx` if built

**Not** `handScores.bin` (535 MB). Only `showdown()` reads it, and the API never
reaches a showdown.

Measured on the development machine against `strategy_740210A.bin` (126M records):
- the first request takes about 100 ms (stage B's tables load), then about 2 ms for
  both the opening node and a flop node;
- the full response is about 480–550 KB of JSON, which gzips to about 14 KB;
- RSS was 168 MB after a few requests (the strategy and index mappings are page
  cache, not RSS).
