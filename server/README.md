# server — the strategy API

Serves the trained strategy over HTTP. Given an action sequence and a board, it
returns the weights, frequencies and visits of all 1326 hole card combinations at
the node that sequence reaches, for the player to act.

## Why it replays

The 64-bit state key is the accumulated residue of `Engine::play()`. Nine of its
fields — `raiseHistoryCompressed`, both `lastActions` nibbles, `villanPosition`,
`relativePosition`, `raisesFaced`, `calls`, `player.calls` and the `hiddenRaise*`
pair — exist nowhere but in that accumulation, and several cannot be recovered
from a snapshot of stacks, bets and pot at all. So the API drives the real
`Engine` through the action list rather than reconstructing a node.

Everything under `server/` reuses `solver/engine.h` unchanged: `deal()`, `play()`,
`newRound()`, `getState()` and `getOptions()` are the solver's own. A
reimplementation would not error on a divergence, it would silently return another
state's weights.

Two facts make the 1326 sweep cheap:

- Hole cards enter the key only through its 20-bit board/meta field, and
  `getState()` never reads a villain's hand. One replay serves all 1326 combos;
  each costs a `classifyHand`, two map lookups and a `getState`.
- `getOptions()` is hand-independent at a node, so legality — and the softmax
  denominator — is computed once.

## Building

From the repository root, always:

```
clang++ -std=c++2b -O2 -I. -o build/indexBuilder    server/indexBuilder.cpp
clang++ -std=c++2b -O2 -I. -pthread -o build/server server/server.cpp -lz
clang++ -std=c++2b -O2 -I. -o build/keyEquivalence  server/tests/keyEquivalence.cpp
clang++ -std=c++2b -O2 -I. -o build/storeProbe      server/tests/storeProbe.cpp
```

On the Oracle VM substitute `g++` (13 or newer) and install `zlib1g-dev`.

`build/indexBuilder` must be run once per strategy file, and again whenever the
solver writes a new one:

```
./build/indexBuilder
```

It writes `solver/strategies/strategy_<spec>.idx` and verifies every key is
reachable through it before returning. The server refuses to start if the index
and the strategy file disagree on the record count.

## Running

```
./build/server --host 127.0.0.1 --port 8080 --warm
```

Must be run from the repository root — `engine.h` resolves the handAnalysis tables
through relative paths. `--warm` faults both mappings in at boot so the first
requests are not disk-bound. `--strategy` and `--index` override the paths;
`--threads` sizes the worker pool.

## Endpoints

### `POST /strategy`

```json
{
  "actions": [2, 0, 0, 0, 0, 0, 1],
  "board": "As7h2d",
  "invTemp": 11.0904,
  "include": { "transition": true, "combos": { "weights": true } }
}
```

| Field | Meaning |
| --- | --- |
| `actions` | Option indices in order: 0 fold, 1 call/check, 2 raise to half pot, 3 raise to one and a half pot, 4 all in. Empty means the first preflop decision, UTG. |
| `board` | `"As7h2d"`, `"As 7h 2d"` or `"As,7h,2d"`. Must hold exactly the cards for the street the actions reach: 0, 3, 4 or 5. |
| `invTemp` | Optional. Defaults to `log(65536)` ≈ 11.0904, the inverse temperature the run finished at. |
| `include` | Which sections to return. **Everything is off unless named**, so a request with no `include` returns `{}`. |

#### `include`

`node`, `transition` and `keyComponents` are booleans. `combos` takes a boolean —
`true` turns on every sub-field — or an object naming `weights`, `frequencies`,
`visits`, `key`, `handMeta`, `counts`. An unknown key is a 400 naming it.

Anything off is **not computed**, not merely not serialised. Measured on one flop
node:

| `include` | Payload | p50 |
| --- | --- | --- |
| `{}` | 2 B | 0.20 ms |
| `{"transition": true}` | 235 B | 0.23 ms |
| `{"combos": {"handMeta": true}}` | 36.8 KB | 0.50 ms |
| `{"combos": {"frequencies": true}}` | 131 KB | 0.72 ms |
| `{"combos": true}` | 334.8 KB | 2.28 ms |

#### The sections

**`node`** — the abstraction's simplified view: `spec`, `round`, `street`, `seat`,
`heads`, `board`, `legal`, `options`, `invTemp`, `weightUnits`.

**`transition`** — exact chips, for a UI applying one action and rendering the
next decision. Only the two seats that changed; the rest of the table accumulates
on the client.

```json
"transition": {
  "pot": 10,
  "lastAction": { "seat": 2, "option": 1, "chips": 395.5, "bet": 0, "inHand": true },
  "actor": { "seat": 2, "chips": 395.5, "bet": 0, "toCall": 0,
             "potAfterCall": 10, "spr": 39.55, "cpr": 0,
             "legal": [false, true, true, true, false] }
}
```

`lastAction` is `null` when `actions` is empty. Note that its `bet` reads 0
whenever that action closed the street — `newRound()` zeroes every live player's
bet, so the chips are in `pot` and the stack is in `chips`.

**`keyComponents`** — the part of the key all 1326 hands share, decomposed into
named fields, with `raiseHistory` (four per-street counts) in place of
`raiseHistoryCompressed`. Values are as the key encodes them: bucketed, clamped,
and zeroed where `MEMORY_BASIC` drops a field. `boardField` and `boardClass` are
`null` preflop, where the board field carries the hand-dependent 169-combo index
instead.

**`combos`** — 1326 rows in hole-pair index order, `i * (101 - i) / 2 + j - 1` for
cards `i < j`, carrying only the requested fields:

```json
[ { "combo": "AcAd", "handMeta": 5, "found": false },
  null,
  { "combo": "KcKd", "found": true,
    "weights": [0.281, -5.841, -5.367, null],
    "frequencies": [0, 1, 3.3e-30, 6.2e-28, 0],
    "visits": 22.5, "nodeVisits": 630, "metaCombos": 28 } ]
```

- A `null` row is a combination that clashes with the board.
- `"found": false` means the state is absent from the table — see below. `found` is
  emitted whenever a record-backed field was requested.
- `weights` holds four numbers for options 1 to 4, stored as `float` and widened
  to double here. There is no weight for folding
  because its pot-normalised return is analytically exactly 0, and that zero is
  the reference every other weight is measured against. Illegal options are
  `null`.
- `frequencies` holds five, fold first, and sums to 1 over the legal options.
- `visits` is **per holding**: `nodeVisits / metaCombos`. The stored count belongs
  to the key, which every combination in the same hand cluster shares, so the raw
  number overstates one holding by the size of its cluster. Preflop `metaCombos`
  is exactly 6 for a pair, 4 suited, 12 offsuit.
- `counts` emits `{found, missing, invalid, metaMissing}` as a sibling of `combos`.

Weights are in units of *(stack change) / (pot after calling)* at that node, so
`transition.actor.potAfterCall` is what turns one back into chips.

Errors are 400 with `error`, a `reason` naming the restriction that blocked the
action, the offending `actionIndex`, and the `legal` set at that point:

```json
{ "error": "option 1 (call) is not legal here",
  "reason": "no preflop limping: the pot is unraised, so every seat must fold or raise",
  "actionIndex": 0,
  "legal": [true, false, true, true, false], "options": [...] }
```

### `GET /health`, `GET /meta`

`/health` reports the loaded record count and mapped bytes. `/meta` reports the
spec string, the config the strategy was trained under, the seat names, the card
and combo encodings, and the default `invTemp`.

## What the tree will not accept

`actions` are checked against `getOptions()` before each is played, so a sequence
taken from a real hand history will often be rejected. The constraints, all from
[engine.h](../solver/engine.h):

- **No preflop limping.** Unraised, every seat must fold or raise.
- **Raise sizes are only** half pot, one and a half pot, and all in, and which are
  offered depends on the SPR at that node.
- **At most six raises** in a hand, the sixth all-in only.
- No flat call facing two or more raises; no third call on a street; no third
  cold caller in a 3-bet-plus or postflop pot.
- Seven seats, 400 starting chips, blinds 1 and 2 — all pinned by the spec string.

There is no projection of an off-tree action onto the nearest legal one. A snapped
node answers a different question from the one asked, and doing it silently would
hide that.

## Missing states

The table holds only the states self-play actually reached. A legitimate node can
be absent, and `visits` on other rows says nothing about it, so a miss is reported
as a miss: `"found": false` on the row, and a `missing` count on the response.

Coverage tracks how plausible the line is. On a heads-up flop after a single raise
and a call, 1173 of 1176 combos are present. On a seven-way river where every seat
saw every street — a line the annealed policy essentially never takes — all 1081
can be absent.

## Tests

```
./build/keyEquivalence 200000 500
```

Self-plays 200k hands with a real `Engine`, weighted toward calls so the walk
reaches the turn and river, and at every node rebuilds the key through
`Replay` + `comboKey` from nothing but the action list and the board. The last run
covered 3,317,259 nodes (2.19M preflop, 560k flop, 306k turn, 258k river) and
6,634 full sweeps with zero key mismatches, zero legal-set mismatches and zero
hand-meta misses. It also checks each sweep's combo accounting and that
frequencies are a distribution over the legal options only.

`build/storeProbe <hex keys...>` prints what the mmap path returns for a key, for
diffing against an independent reader of `strategy_<spec>.bin`.

## On the VM

**Step by step instructions are in [DEPLOY.md](DEPLOY.md).** In summary, copy
from the repository root:

- `solver/handAnalysis/boardClasses.bin`, `handMetas.bin`, `handMetasBasic.bin` (178 MB)
- `solver/strategies/strategy_<spec>.bin` (8 + 28 bytes per record)

**Not** `handScores.bin` (535 MB): only `showdown()` reads it and the API never
reaches showdown, which is why `server.cpp` calls `Engine::loadTables(false)`.

Then run `indexBuilder`, install `deploy/firefly-api.service` and
`deploy/nginx.conf`, and open 443 in the Oracle security list *and* in the VM's
own firewall.

Steady state is roughly 312 MB resident for the board-class table and the two hand
meta maps — fixed, independent of strategy size — plus ~750 MB of file-backed
mappings for the strategy and its index, which grow with the strategy and are
reclaimable page cache rather than RSS.

Measured on the development machine: p50 1.8 ms for a node where most combos are
present, 0.5 ms where most miss (a miss resolves on the index tag without touching
the record file). The response is ~267 KB of JSON, ~7.5 KB gzipped.
