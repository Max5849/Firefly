# server — the strategy API

Serves the trained range strategy over HTTP. Given a config, an action sequence and
a board, it returns three things at the node that sequence reaches:

- **`game`**: the table as a player sees it. Seats, stacks, bets, pot, board, whose
  turn it is, what they may do and for how much, and the hand so far.
- **`key`**: the acting seat's state key, decoded field by field from its own bits.
- **`spot`**: for all 1326 hole card combinations, the actions, the acting seat's
  range (reach), the EV in chips, and the visits.

## Why it replays

The 64-bit state key is the accumulated residue of `Engine::play()`. Most of its
fields (`raiseHistoryCompressed`, the story digits, `villanPosition`,
`relativePosition`, `raisesFaced`, the two `calls` counters, the raise descriptors
and both raise action positions) exist nowhere but in that accumulation. So the API
replays the action list through an engine rather than reconstructing a node.

That engine is `TableEngine<P>` in [engine/tableEngine.h](engine/tableEngine.h). It
holds the game rules of [solver/multiHand/engine.h](../solver/multiHand/engine.h)
(deal, play, setActionState, getOptions, beginNewRound, the meta state) for any table
size from 3 to 7. It leaves out the trainer's rewards, line odds and card removal
sums. The trainer's engine fixes `PLAYER_COUNT` at compile time and is left
untouched.

Because the rules are a copy, `build/keyEquivalence` plays `TableEngine<PLAYER_COUNT>`
in lockstep with the trainer's `Engine` and fails on any difference. **A rule change
in solver/multiHand/engine.h has to be copied into tableEngine.h.**
[engine/tables.h](engine/tables.h) loads the lookup tables through `Engine`'s own
loaders, minus `handScores.bin`.

A range is the same kind of residue. Every combo's reach is the product of its
seat's own `INV_TEMP` policy along the line. So at every past decision the replay
looks up the acting seat's nodes in the strategy and narrows the range with
`TableEngine::applyPolicy`, the range pass of `Explorer::chooseRangeOption`
(RANGE_FLOOR included). A
node with **no record drops the combos that reach it**, and the response counts
those drops (`spot.replayDrops`).

The replay uses the server's compiled `INV_TEMP` and `RANGE_FLOOR`. A strategy
trained with other values replays differently.

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
worker pool. `--warm` faults each strategy's mappings in when it is first opened.

Strategy files are opened **on first request** for their spec, and stay open after
that. If `strategy_<spec>.idx` is missing or does not match the `.bin` file, the
server rebuilds it beside the strategy file. That takes about a second at 15M
records. `build/indexBuilder` does the same ahead of time:

```
./build/indexBuilder                                        # the spec config.h names
./build/indexBuilder --players 3 --chips 200 --metas 1 --memory 1
```

## Endpoints

### `POST /strategy`

```json
{
  "config": { "players": 3, "chips": 200, "metasBasic": true, "memoryBasic": true },
  "actions": [2, 1, 1],
  "board": "As7h2d",
  "include": { "game": true, "key": true, "spot": true }
}
```

| Field | Meaning |
| --- | --- |
| `config` | Which strategy to read. Each field is optional and defaults to `config.h`. `players` is 3–7. `chips` must be a positive even integer, because the spec `10000·players + 10·chips + 10·metasBasic + memoryBasic` is ambiguous for odd stacks. |
| `actions` | Option indices in order: 0 fold, 1 call/check, 2 raise to half pot, 3 raise to one and a half pot, 4 all in. Empty means the first preflop decision (seat `3 % players`). |
| `board` | `"As7h2d"`, `"As 7h 2d"` or `"As,7h,2d"`. It must hold exactly the cards for the street the actions reach: 0, 3, 4 or 5. |
| `include` | Optional. Leaving it out means all three sections. When present, only the sections set to `true` are returned. |

An unknown field or section is a 400 that names it.

#### `game`

```json
{ "config": {"players": 3, "chips": 200, "metasBasic": true, "memoryBasic": true, "spec": "32011"},
  "round": 1, "street": "flop", "board": "As7h2d", "pot": 13.5, "heads": 3,
  "seats": [{"seat": 0, "name": "BTN", "stack": 195.5, "bet": 0, "inHand": true,
             "allIn": false, "acting": false}, ...],
  "actor": {"seat": 1, "name": "SB", "stack": 195.5, "bet": 0, "toCall": 0,
            "potAfterCall": 13.5, "spr": 14.48, "cpr": 0},
  "legal": [false, true, true, true, false],
  "options": [{"option": 2, "name": "bet", "size": "0.5x pot", "legal": true,
               "betTo": 6.75, "adds": 6.75, "reason": ""}, ...],
  "history": [{"kind": "post", "seat": 1, "name": "small blind", "betTo": 1, ...},
              {"kind": "action", "seat": 0, "name": "raise", "option": 2, "round": 0,
               "betTo": 4.5, "adds": 4.5, "stack": 195.5},
              {"kind": "street", "round": 1, "street": "flop", "board": "As7h2d"}] }
```

- `pot` counts every chip committed, including this street's bets.
- `seats[].bet` is the amount in front on the current street.
- Option `name` is what the table would say: check or call, and bet or raise.
- `betTo` comes from `TableEngine::betTo`, which repeats `play()`'s float
  operations; the test asserts every raise lands on it.
- Illegal options carry the `reason` `getOptions()` turned them off for.
- Seat names follow the player count: BTN, SB, BB, then the last `P − 3` of UTG, LJ,
  HJ, CO.

#### `key`

The shared key is `actionState | boardField << 42`. It covers bits 0–57: every bit
except the hand cluster (58–63), which differs per combo and is given in each `spot`
row. For each field the section lists:

- `shift` and `width`
- `bits`, the field's binary, most significant bit first
- `value`, the raw number those bits hold
- `meaning`, in words
- `zeroed`, set when `memoryBasic` forces the field to 0
- `parts`, for packed fields:
  - `raiseHistory` is ranked back to the four per-street raise counts
  - `story` splits into its base-3 street digits plus the hero and villain
    positions
  - `actionPositions` splits into three buckets

Preflop, bits 42–57 hold each combo's 169 class, so the board field is
`"shared": false`.

The section also carries `heroSeat`, `villanSeat` (needed to read the villain
digits, but not stored in the key), and the exact `spr` and `cpr`.

#### `spot`

```json
{ "legal": [...], "potAfterCall": 13.5,
  "totalVisits": 2081228, "records": 27, "found": 1142, "missing": 34, "invalid": 150,
  "rangeCombos": 1110, "rangeWeight": 95.69, "rangeMix": [0, 0.84, 0.078, 0.081, 0],
  "rangeEv": 0.14, "emptySeats": [], "replayDrops": {"decisions": 0, "groups": 0, "combos": 0},
  "combos": [null, {"combo": "KcQd", "reach": 0.3345, "cluster": 8, "key": "0x200af40018001521",
                    "found": true, "clusterCombos": 41,
                    "weights": [-0.627, -1.194, -1.736, null],
                    "frequencies": [0, 0.957, 0.041, 0.002, 0],
                    "evOptions": [null, -8.47, -16.12, -23.44, null], "ev": -8.81,
                    "visits": 185510}, ...] }
```

- **Rows** are in hole-pair index order, `i * (101 - i) / 2 + j - 1` for cards
  `i < j`. A row is `null` when the combo uses a board card. On a row with
  `"found": false`, the node has no record, so only `reach`, `cluster`, `key` and
  `clusterCombos` are present.
- **`reach`** is the acting seat's: the product of its own policy along the line,
  after `RANGE_FLOOR`. It is 0 for a combo the line has ruled out, which still
  carries its strategy.
- **`weights`** are the record's four values for options 1–4, pot-normalised and
  relative to folding, which is exactly 0. An illegal option is `null`.
- **`frequencies`** is the `INV_TEMP` softmax over the legal options, fold first.
- **`evOptions`** gives each option's value in **chips relative to folding**:
  `weight × potAfterCall`. `ev` is the policy-weighted mean.
- **`visits`** is the record's count, from the cooldown block and reach-weighted.
  It is shared by every combo in the cluster, and `clusterCombos` is how many valid
  combos share it.
- **`totalVisits`** sums the distinct records the acting range reaches (combos
  with reach > 0).
- **`rangeMix`** and **`rangeEv`** are reach-weighted over the combos that have a
  record.
- **`emptySeats`** lists seats whose range the line has emptied. Training drops
  such a hand at the next street.

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
| 500 | `strategyUnreadable`, `internal` | | a corrupt file, or an unexpected throw |

### `GET /meta`, `GET /health`

`/meta` returns:
- the default config
- the limits
- the `available` strategies, decoded from `solver/strategies/strategy_*.bin`
- seat names per player count
- `INV_TEMP`, `RANGE_FLOOR`, the blinds, the option names and the card encoding

`/health` lists the specs opened so far and their record counts.

## What the tree will not accept

`actions` are checked against `getOptions()` before each one is played:

- **No preflop limping.** In an unraised pot, every seat must fold or raise.
- **Raises are only** half pot, one and a half pot, or all in. Which of these are
  offered depends on the SPR at that node.
- **At most six raises** per hand, and the sixth can only be all in.
- No flat call when facing three or more raises since you last acted. No third call
  on a street. No third cold caller in a 3-bet-plus or postflop pot.

An off-tree action is not projected onto the nearest legal one.

## Tests

```
./build/keyEquivalence 4000
```

The test runs in two parts.

1. **Lockstep.** The trainer's `Engine` and `TableEngine<PLAYER_COUNT>` play the same
   hands. At every decision, both acting ranges are narrowed by the same random
   per-group policy: the trainer's through a copy of `chooseRangeOption`'s range
   pass, the server's through `applyPolicy`. The two must agree on the action state,
   the options, every seat's chips, bets and range, what `play()` and
   `beginNewRound()` return, and every meta the trainer classifies.
2. **Every table size from 3 to 7.** `TableEngine` self-plays under random
   `metasBasic`, `memoryBasic` and stack sizes, with calls weighted up so the walk
   reaches the turn and river. At every node the test asserts that:
- `Replay`, given only the action list and the revealed board, reaches the same
  action state, legal set and per-combo meta state;
- every `key` field equals the engine quantity it encodes, and the fields repack
  into the shared key;
- every illegal option has a reason and no legal one does;
- every raise lands on its `betTo`.

At 4000 hands this covers 34k lockstep decisions and 218k nodes, with 0 failures.
Doubling the range floor in `TableEngine` fails the lockstep part on the first
narrowed street.

`build/storeProbe [--spec 32011] <hex keys...>` prints what the mmap path returns for
a key.

## On the VM

**Step-by-step instructions are in [DEPLOY.md](DEPLOY.md).** Copy these from the
repository root:

- `solver/handAnalysis/boardClasses.bin`, `boardClassesBasic.bin`,
  `handMetas.bin` and `handMetasBasic.bin`
- every `solver/strategies/strategy_<spec>.bin` to serve (8 + 28 bytes per record)

**Not** `handScores.bin` (535 MB). Only `setRewards()` reads it, and the API never
reaches a showdown, so `ServerTables::load()` skips it.

Measured on the development machine against `strategy_32011.bin` (15.3M records):
- p50 is about 1 ms for both the opening node and a flop node;
- the full response is about 460 KB of JSON, which gzips to 15–27 KB;
- RSS was 464 MB after a few requests.
