# TashtegoGTO — the strategy browser

A keyboard-driven front end for `POST /strategy`. Type a line, watch all 1326
combinations respond. Every action, board card and reset is a keystroke; the only
controls are the config bar in the header, which picks the strategy file.

The page has three columns:

- **left — the table.** Street, pot, board, every seat's stack and bet and whether
  it has folded or is all in, the hand so far, and the acting seat's options with
  their sizes. All of it comes from the server's `game` section.
- **centre — the grid.** Every combination's action mix, filled to its reach.
- **right — the spot and the key.** The range's visits, size, action mix and EV;
  the hovered combination's reach, frequency and EV per option, visits and key;
  and the acting seat's state key, bit by bit, with each field decoded.

```
npm install
npm run dev        # http://localhost:5173
```

It needs the API on `127.0.0.1:8080`, which is either

```bash
cd .. && ./build/server --port 8080          # local, with the tables on this machine
ssh -N -i ~/.ssh/key -L 8080:127.0.0.1:8080 ubuntu@<VM_IP>   # or the VM, DEPLOY.md section 9
```

`API_PORT=8081 npm run dev` points the proxy at another port, so a local server can
run beside a tunnel that holds 8080. Vite proxies `/api` to that port, so the page is same-origin and needs neither CORS
nor a token. To point at a deployed server instead, copy `.env.example` to `.env`
and set `VITE_API_URL` and `VITE_API_TOKEN` — and note that a token in the bundle is
readable by anyone who can open the page. The matching `Access-Control-Allow-Origin`
lives in `../server/deploy/nginx.conf`, off until you name an origin in the
`$cors_origin` map.

## The keyboard

| Key | Acting | Entering a board |
| --- | --- | --- |
| `f` `c` `s` `b` `a` | fold, call, small, big, all in | — |
| `2`–`9` `t` `j` `q` `k` `a` | — | rank |
| `s` `h` `d` `c` | — | suit, completing the card |
| `Backspace` | undo the last action | undo a card, then the held action |
| `[` `]` | lower / raise the inverse temperature | same |
| `n` | new hand | — |

Only legal options are offered; the keycaps at the foot of the left column are the
legality display, so a key that is not shown does nothing. `a` and `c` mean
different things in the two modes, which never overlap.

## Choosing a strategy

The header picks players (3–7), starting chips (even, blinds 1/2), and basic or
full metas and memory. Those four name the strategy file, `strategy_<spec>.bin`;
the spec beside them turns red when nothing has been trained for it, and a banner
lists what has. The `trained` menu jumps straight to any file the server has.
Changing the config starts a new hand.

## Temperature

`temp` in the header sets the inverse temperature **per chip of EV**, ln(2^k) for k
from 1/64 to 4. Each step multiplies k by 2^(1/8), and `[` and `]` step it. Every policy
is a softmax over each option's EV in chips, so at ln(2^k) an option one chip better is
played 2^k times as often, whatever the pot. The default is k = 1.

It changes the whole page at once: the policy each earlier decision narrowed its range
with, and the policy shown now. Changing it re-asks for the current node, and undo or
a ledger jump re-asks for any node fetched at another temperature.

## Reading the grid

Each of the 169 cells is one row of columns, one per combination — 6 for a pair, 4
suited, 12 offsuit. **A column's height is the combination's reach**: how much of
it is still in the acting seat's range, the product of that seat's own policy along
the line. A full column is a hand that always plays this way to here; an empty one
is a hand the line has ruled out. Inside that height the column fills from the
bottom in action order:

| | fold | call | raise | raise | all in |
| --- | --- | --- | --- | --- | --- |
| | blue | green | pink | red | purple |

**Columns are ordered by how close the hand is to a flush**, most first. For each
combination, count the board cards plus hole cards in each suit the hole cards
actually contribute to, and take the largest; ties go to the hand whose card in that
suit is higher. On a two-spade flop every A♠5x therefore sits left of every Ax5♠ —
both hold three spades, and the ace outranks the five. The order changes with the
board, which is the point: it is the board that decides what "suited" is worth.

**Eliminated combinations are black and shift to the right**, so the live columns stay
together on the left. Three separate things eliminate a column, and the grid does
not distinguish them:

- the combination uses a board card;
- the strategy has no record of that node;
- the hand cluster is too thin to read — its visit count, divided by the number of
  combinations in the cluster, is under 5% of the largest such figure at this node.

That last one is normalised deliberately. A cluster covering 255 holdings would
otherwise always outrank one covering 12 on raw counts alone.

`VISITS` is the node total: the sum over the *distinct* records the acting range
reaches, not the sum of per-combination figures, which would multiply each cluster
by its size. EVs are in chips relative to folding.

## Street changes

The server cannot replay past a street without its cards, so a street rollover is
announced as a rejected request. Sending an action that closes a street returns

```
400 {"code": "needsBoard", "street": "flop", "cards": 3, ...}
```

and the client reads that as "hold the action, ask for three cards". A 400 per
street in the network log is normal.

## Layout

```
src/
  api/     client.ts   the fetch layer; errors classified by their code
           types.ts    the response contract: game, key, spot
  hand/    useHand.ts  the state machine: config, frames, board entry, undo
           keys.ts     the keymap
  grid/    layout.ts   169 cells -> 1326 strips, asserted complete at load
           metas.ts    the thin-cluster cut
           RangeGrid.tsx
  ui/      ConfigBar.tsx    players, chips, metas, memory
           ActionColumn.tsx table, seats, ledger, keycaps
           SpotColumn.tsx   spot summary, hover readout, key panel
```
