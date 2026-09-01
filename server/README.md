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
{ "actions": [2, 0, 0, 0, 0, 0, 1], "board": "As7h2d", "invTemp": 11.0904 }
```

| Field | Meaning |
| --- | --- |
| `actions` | Option indices in order: 0 fold, 1 call/check, 2 raise to half pot, 3 raise to one and a half pot, 4 all in. Empty means the first preflop decision, UTG. |
| `board` | `"As7h2d"`, `"As 7h 2d"` or `"As,7h,2d"`. Must hold exactly the cards for the street the actions reach: 0, 3, 4 or 5. |
| `invTemp` | Optional. Defaults to `log(65536)` ≈ 11.0904, the inverse temperature the run finished at. |

The response describes the node and then carries 1326 rows in hole-pair index
order, `i * (101 - i) / 2 + j - 1` for cards `i < j`:

```json
{
  "spec": "74011", "round": 1, "street": "flop", "seat": 2, "heads": 2,
  "board": "As7h2d",
  "pot": 10, "potAfterCall": 10, "toCall": 0, "chips": 396, "spr": 39.55, "cpr": 0,
  "legal": [false, true, true, true, false],
  "options": ["fold", "call", "raise 0.5x pot", "raise 1.5x pot", "all in"],
  "invTemp": 11.0904,
  "found": 1173, "missing": 3, "invalid": 150, "metaMissing": 0,
  "combos": [
    { "combo": "AcAd", "key": "0x684150a3c6a10", "found": false },
    null,
    { "combo": "KcKd", "key": "0x68415163c6a10", "found": true,
      "weights": [0.281, -5.841, -5.367, null],
      "frequencies": [0, 1, 3.3e-30, 6.2e-28, 0],
      "visits": 630 }
  ]
}
```

- A `null` row is a combination that clashes with the board.
- `"found": false` means the state is absent from the table — see below.
- `weights` holds four numbers for options 1 to 4. There is no weight for folding
  because its pot-normalised return is analytically exactly 0, and that zero is
  the reference every other weight is measured against. Illegal options are
  `null`, since their stored slot holds whatever was in memory.
- `frequencies` holds five, fold first, and sums to 1 over the legal options.
- Weights are in units of *(stack change) / (pot after calling)* at that node, so
  `potAfterCall` is what turns one back into chips.

Errors are 400 with `error`, the offending `actionIndex`, and the `legal` set at
that point:

```json
{ "actionIndex": 0, "error": "option 1 (call) is not legal here",
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
- `solver/strategies/strategy_<spec>.bin` (477 MB)

**Not** `handScores.bin` (535 MB): only `showdown()` reads it and the API never
reaches showdown, which is why `server.cpp` calls `Engine::loadTables(false)`.
Not `strategyBase_<spec>.bin` either — nothing here reads it.

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
