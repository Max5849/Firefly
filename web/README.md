# TashtegoGTO — the strategy browser

A keyboard-driven front end for `POST /strategy`. Type a line, watch all 1326
combinations respond. There are no buttons: every action, board card and reset is a
keystroke.

```
npm install
npm run dev        # http://localhost:5173
```

It needs the API on `127.0.0.1:8080`, which is either

```bash
cd .. && ./build/server --port 8080          # local, with the tables on this machine
ssh -N -i ~/.ssh/key -L 8080:127.0.0.1:8080 ubuntu@<VM_IP>   # or the VM, DEPLOY.md section 9
```

Vite proxies `/api` to that port, so the page is same-origin and needs neither CORS
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
| `n` | new hand | — |

Only legal options are offered; the keycaps at the foot of the left column are the
legality display, so a key that is not shown does nothing. `a` and `c` mean
different things in the two modes, which never overlap.

## Reading the grid

Each of the 169 cells is one row of columns, one per combination — 6 for a pair, 4
suited, 12 offsuit. Every column fills from the bottom in action order:

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
- the solver never built that node;
- the hand class is too thin to read — its visit count, divided by the number of
  combinations in the class, is under 5% of the largest such figure at this node.

That last one is normalised deliberately. A class covering 255 holdings would
otherwise always outrank one covering 12 on raw counts alone. On a `Ks9s2dAh` turn
it blacks out the ace-queen class at 28,030 visits over 99 combinations — 283 each,
against a 367 cut — while a class with 26,914 raw visits over 36 combinations
survives at 748.

`VISITS` in the header is the node total: the sum over *distinct* hand classes, not
the sum of the per-combination figures, which would multiply each class by its size.

## Street changes

The server validates the board count after replaying the actions, so a street
rollover is only ever announced as a rejected request. Sending an action that closes
a street returns

```
400 the action list reaches the flop, which needs 3 board cards, but 0 were given
```

and the client reads that as "hold the action, ask for three cards". Two 400s per
hand in the network log are normal.

## Layout

```
src/
  api/     client.ts   the fetch layer and its error classifier
           types.ts    the response contract, including all four combo shapes
  hand/    useHand.ts  the state machine: frames, board entry, undo
           keys.ts     the keymap
  grid/    layout.ts   169 cells -> 1326 strips, asserted complete at load
           RangeGrid.tsx
  ui/      ActionColumn.tsx  specs, ledger, keycaps
```

`keyComponents`, `weights` and the state keys are fetched but not yet rendered.
