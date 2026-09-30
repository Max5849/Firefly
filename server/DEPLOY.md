# Deploying the strategy API to an Oracle VM

Written to be followed line by line. Every command says which machine to run it
on: **[MAC]** or **[VM]**.

---

## 0. The mental model

Three things have to end up on the VM.

| What | Where it comes from | Size |
| --- | --- | --- |
| Source code | `git clone` from GitHub | 1.5 MB |
| Data tables | copied from your Mac — they are **not** in git | 451 MB |
| Compiled binaries | built **on the VM** — they cannot be copied | — |

`.gitignore` excludes `*.bin`, so none of the big tables are in the repo. That is
why cloning gives you a working tree with no data in it, and why the copy step
exists.

Once running, a request passes through three gates:

```
your laptop --443--> [Oracle security list] -> [the VM's own firewall] -> nginx -> 127.0.0.1:8080 (the server)
                          cloud rule              host rule               TLS + token      binds localhost only
```

Both firewalls have to be opened. Forgetting the second one is the single most
common reason an Oracle VM times out on a port you "already opened".

---

## 1. Do I need Oracle API Gateway?

**No.**

API Gateway is a separate Oracle managed service that sits in front of a backend
and does routing, authentication and rate limiting. It costs money, adds a second
thing to configure, and would still need the VM reachable behind it.

You already have all of that:

- **TLS** — nginx with a Let's Encrypt certificate.
- **Authentication** — nginx checks `Authorization: Bearer <token>`.
- **The backend** — the server binds `127.0.0.1` only, so nothing but nginx on the
  same machine can reach it, no matter what the firewalls say.

What you *do* need from Oracle is one **ingress rule in the subnet's security
list**. That is a firewall rule, not a gateway. Section 9.

---

## 2. Connect to the VM

**[MAC]**

```bash
ssh -i ~/.ssh/your-oracle-key ubuntu@<VM_PUBLIC_IP>
```

The login user depends on the image: `ubuntu` on Ubuntu images, `opc` on Oracle
Linux images. Everything below assumes `ubuntu` — substitute if yours differs.

Check what you are on:

```bash
cat /etc/os-release | head -2
uname -m          # expect aarch64
free -g           # expect ~12 GB
df -h /           # need ~2 GB free; 451 MB of data + 512 MB index + binaries
```

---

## 3. Install the build tools

The compiler must be **GCC 11 or newer** — the code uses `std::barrier`,
`std::bit_cast` and `std::to_chars`, which older GCC does not have.

**[VM] — Ubuntu 24.04 (recommended)**

```bash
sudo apt update
sudo apt install -y g++ zlib1g-dev git rsync
g++ --version      # expect 13.x
```

**[VM] — Ubuntu 22.04**

```bash
sudo apt update
sudo apt install -y g++-12 zlib1g-dev git rsync
g++-12 --version   # use g++-12 everywhere below instead of g++
```

**[VM] — Oracle Linux 9**

```bash
sudo dnf install -y gcc-toolset-13 zlib-devel git rsync
scl enable gcc-toolset-13 bash    # run this in every new shell before building
g++ --version                     # expect 13.x
```

`zlib` is what lets the server gzip its responses. Without the dev package the
build fails at the link step with `cannot find -lz`.

---

## 4. Clone the repo

The repo is **private**, so the VM needs its own read access. A deploy key is the
right tool: it is a read-only SSH key scoped to this one repository, so a
compromised VM cannot push or reach your other repos.

**[VM] — make the key**

```bash
ssh-keygen -t ed25519 -C "firefly-vm" -f ~/.ssh/id_ed25519 -N ""
cat ~/.ssh/id_ed25519.pub
```

Copy the line it prints.

**[MAC, in a browser]** — go to
`https://github.com/Max5849/Firefly/settings/keys` → **Add deploy key** → paste it
→ title "oracle vm" → leave *Allow write access* **unchecked** → Add key.

**[VM] — clone into /opt/firefly**

```bash
sudo mkdir -p /opt/firefly
sudo chown $USER:$USER /opt/firefly
git clone git@github.com:Max5849/Firefly.git /opt/firefly
cd /opt/firefly
ls solver server
```

`/opt/firefly` is not arbitrary — the systemd unit sets it as the working
directory, and `engine.h` finds its tables through paths relative to the
repository root. If you put the repo somewhere else, change `WorkingDirectory` and
`ExecStart` in the unit file to match.

> **Do you need only part of the repo?** No — the whole clone is 1.5 MB, because
> every large file is gitignored. There is nothing to trim. `git clone` is the
> whole story.

**[VM] — make the two directories git could not create**

Both are gitignored, so they do not exist in a fresh clone, and the next two steps
fail confusingly without them:

```bash
mkdir -p /opt/firefly/build
mkdir -p /opt/firefly/solver/strategies
```

---

## 5. Copy the data tables from your Mac

Run this **on the Mac**, from the repository root. It pushes to the VM.

```bash
cd ~/Poker/Firefly

rsync -avP -e "ssh -i ~/.ssh/your-oracle-key" \
  solver/handAnalysis/boardClasses.bin \
  solver/handAnalysis/handMetas.bin \
  solver/handAnalysis/handMetasBasic.bin \
  ubuntu@<VM_PUBLIC_IP>:/opt/firefly/solver/handAnalysis/

rsync -avP -e "ssh -i ~/.ssh/your-oracle-key" \
  solver/strategies/strategy_74011.bin \
  ubuntu@<VM_PUBLIC_IP>:/opt/firefly/solver/strategies/
```

`-P` shows progress and lets you resume: if the transfer drops, run the same
command again and it picks up where it stopped.

451 MB over a home upload link takes a while — roughly 6 minutes at 10 Mbit/s,
70 seconds at 50 Mbit/s.

**Do not copy** `handScores.bin` (510 MB). The API never reaches showdown, so it is
dead weight — that is exactly what the `Engine::loadTables(false)` change is for.
There is no `strategyBase_*.bin` any more: a run writes one file, and a run that uses
a base reads a previous `strategy_<spec>.bin`.

**[VM] — check the copy landed intact**

```bash
cd /opt/firefly
stat -c "%s  %n" solver/handAnalysis/*.bin solver/strategies/*.bin
```

| File | Bytes, exactly |
| --- | --- |
| `solver/handAnalysis/boardClasses.bin` | 121859400 |
| `solver/handAnalysis/handMetas.bin` | 27899243 |
| `solver/handAnalysis/handMetasBasic.bin` | 27899243 |
| `solver/strategies/strategy_74011.bin` | 8 + 28 × records (was 44 × records) |

You do not have to be careful here: every one of these files has its exact
expected size checked at startup, and a wrong size is a clear error on the spot
rather than silently wrong answers later.

---

## 6. Build — and why you cannot copy my binaries

> **The question: my Mac is ARM and the VM is ARM, so can I just copy the built
> files across?**
>
> **No.** Same CPU family is not enough. A program also has to match the operating
> system's *executable format* and its *system libraries*, and those are
> completely different:
>
> | | Your Mac | The Oracle VM |
> | --- | --- | --- |
> | CPU | ARM64 | ARM64 ✅ same |
> | Format | Mach-O | ELF ❌ |
> | Standard library | LLVM libc++ | GNU libstdc++ ❌ |
> | System calls | Darwin/XNU | Linux ❌ |
>
> Linux would look at a Mach-O file and refuse to run it — it does not recognise
> the format at all. Think of it as the same engine in a car with a different
> chassis, wiring loom and fuel: the engine fits nothing else.
>
> Building on the VM takes about a minute. Just build there.

**[VM]**

```bash
cd /opt/firefly
mkdir -p build

g++ -std=c++2b -O2 -I. -o build/indexBuilder    server/indexBuilder.cpp
g++ -std=c++2b -O2 -I. -pthread -o build/server server/server.cpp -lz
```

`server.cpp` pulls in 35,000 lines of vendored HTTP and JSON headers, so it takes
roughly a minute on one A1 core. That is normal, not a hang.

Optional, and worth it once — the correctness harness:

```bash
g++ -std=c++2b -O2 -I. -o build/keyEquivalence server/tests/keyEquivalence.cpp
```

Two things about that command line, both load-bearing:

- `-I.` plus running from `/opt/firefly` — every `#include` in this project is
  written from the repository root, as `"solver/engine.h"`.
- `-lz` on the server only — that is zlib, for gzipping responses.

---

## 7. Build the index

**[VM]**

```bash
cd /opt/firefly
./build/indexBuilder
```

This reads `strategy_74011.bin` and writes a 256 MB `strategy_74011.idx` beside
it — a hash table that turns a state key into a record without scanning 10.8
million of them. It verifies every key is reachable before it exits:

```
Records: 10843356
Slots: 33554432 (256.000 MB, load 32.316%)
Mean probes per insert: 1.239
Wrote solver/strategies/strategy_74011.idx
Verified 10843356 keys
```

**Run this again every time you copy over a new strategy file.** The server
refuses to start if the index and the strategy disagree, so a stale index is an
error rather than wrong answers.

---

## 8. Smoke test, before any networking

**[VM]**

```bash
cd /opt/firefly
./build/server --port 8080 &
sleep 5

curl -s localhost:8080/health
curl -s -X POST localhost:8080/strategy \
  -d '{"actions":[],"include":{"combos":{"frequencies":true}}}' | head -c 400
```

Expect `{"status":"ok", ...}` and then a wall of JSON whose first combo is `AcAd`
raising about 94% of the time. If that works, the hard part is done — everything
left is networking.

Note the `include`: every response section is off unless asked for, so
`{"actions":[]}` on its own returns `{}`. That is a successful reply — it means the
action list replayed legally — but it is not much of a smoke test.

Optionally prove the port is exactly right:

```bash
./build/keyEquivalence 20000 2000 | tail -4     # expect PASSED, 0 mismatches
```

Stop it before moving on:

```bash
kill %1
```

---

## 9. The fastest safe route: an SSH tunnel

**Before doing any TLS work, get it usable this way.** It needs no domain, no
certificate, and no firewall changes at all, because it rides the SSH connection
you already have.

**[VM]** — install the service so it is always running:

```bash
sudo cp /opt/firefly/server/deploy/firefly-api.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now firefly-api
systemctl status firefly-api --no-pager
```

If your login user is not `ubuntu`, edit `User=` and `Group=` in
`/etc/systemd/system/firefly-api.service` first, then `daemon-reload` again.

**[MAC]** — open the tunnel and use it:

```bash
ssh -N -i ~/.ssh/your-oracle-key -L 8080:127.0.0.1:8080 ubuntu@<VM_PUBLIC_IP>
```

Leave that running. In another terminal:

```bash
curl -s localhost:8080/health
curl -s -X POST localhost:8080/strategy \
  -H 'Content-Type: application/json' \
  -d '{"actions":[2,0,0,0,0,0,1],"board":"As7h2d"}' | head -c 400
```

Nothing is exposed to the internet. If you are the only consumer, you can stop
here permanently.

### 9a. What the flags mean, and which direction is which

```
ssh -N -i ~/.ssh/key -L 8080:127.0.0.1:8080 ubuntu@<VM_PUBLIC_IP>
        │              │  │    │         └── the port on the VM's own loopback
        │              │  │    └── the host, resolved *by the VM* - so 127.0.0.1 is the VM
        │              │  └── the port opened on your Mac
        │              └── local forward: your Mac reaches into the VM
        └── no shell, just the forward
```

`-L` is the direction you want: it opens port 8080 **on your Mac** and carries
anything sent there into the VM, where it comes out at `127.0.0.1:8080` — which is
exactly where the server binds. To the server the request looks local, which is why
this needs no firewall rule and no token.

`-R` is the opposite direction and is **not** used here. It would open a port on the
VM and forward it back to your Mac, which is what you would want if the VM had to
reach something running on your laptop. Reaching for `-R` when you meant `-L` gives a
tunnel that connects and then refuses every request.

Ports do not have to match. If 8080 is already taken on your Mac, move the left-hand
number only, and point clients at that instead:

```bash
ssh -N -i ~/.ssh/key -L 9090:127.0.0.1:8080 ubuntu@<VM_PUBLIC_IP>
curl -s localhost:9090/health
```

### 9b. Backgrounding it, checking it, closing it

The command above occupies a terminal. `-f` sends it to the background instead:

```bash
ssh -f -N -i ~/.ssh/your-oracle-key -L 8080:127.0.0.1:8080 ubuntu@<VM_PUBLIC_IP>
```

There is no message when that succeeds — `-f` is silent by design. Check it:

```bash
pgrep -fl "8080:127.0.0.1:8080"     # the tunnel process, if any
lsof -nP -iTCP:8080 -sTCP:LISTEN    # something is listening on 8080
curl -s localhost:8080/health       # ... and it is the API
```

Match on the forward specification, not on `ssh -N -L`. A pattern that starts with a
dash is read as an option by `pkill`, and `\?`-style optional groups are not valid in
the regex flavour BSD `pgrep` uses — both fail by matching **nothing**, which reads
identically to "no tunnel is running".

Only the third proves the whole path. The first two go green even if the API on the
far side is stopped, in which case `curl` returns **connection reset** rather than
refused: the tunnel accepted your connection and the VM end had nothing to hand it
to. That distinction is the fastest way to tell a dead tunnel from a dead service.

| `curl localhost:8080/health` says | Meaning |
| --- | --- |
| `{"status":"ok",...}` | working |
| connection refused | no tunnel — nothing is listening on your Mac |
| connection reset by peer / empty reply | tunnel is up, `firefly-api` is down on the VM |
| hangs | the SSH connection has died without closing; kill it and reopen |

Closing it:

```bash
# foreground: Ctrl-C in its terminal
pkill -f "8080:127.0.0.1:8080"      # background: by what it forwards
```

Kill it by the forward specification, not by the port. `lsof -ti:8080 | xargs kill`
would also match every **client** that happens to have a connection open to that
port, and on a Mac that usually includes your browser.

### 9c. Surviving a dropped connection

A laptop that sleeps, or a network that changes, kills the tunnel silently — the
process often stays alive holding a socket that no longer works, which is the "hangs"
row above. Two ways to make that less annoying.

**Keepalives**, so a dead connection is noticed and dropped rather than hanging.
Add to `~/.ssh/config` on the Mac:

```
Host firefly
    HostName <VM_PUBLIC_IP>
    User ubuntu
    IdentityFile ~/.ssh/your-oracle-key
    LocalForward 8080 127.0.0.1:8080
    ServerAliveInterval 30
    ServerAliveCountMax 3
    ExitOnForwardFailure yes
```

The tunnel is then one word, and it refuses to sit there pretending to work if the
forward could not be established:

```bash
ssh -N firefly
```

**Automatic reconnection**, if you want it to heal itself:

```bash
brew install autossh
autossh -M 0 -f -N firefly
```

`-M 0` turns off autossh's own monitoring port and leans on the `ServerAlive`
settings above, which is the combination that actually works over a flaky link. Stop
it with `pkill autossh` — killing the `ssh` child alone just makes autossh start a
new one.

---

## 10. Public HTTPS: what you need first

To go public you need **a domain name pointing at the VM**. Let's Encrypt will not
issue a certificate for a bare IP address.

- Have a domain? Add an **A record** for `api.yourdomain.com` → your VM's public
  IP.
- Don't have one? [DuckDNS](https://duckdns.org) gives a free subdomain like
  `yourname.duckdns.org` in about a minute, and Let's Encrypt accepts it.

Confirm DNS resolves before continuing, or certbot will fail:

```bash
dig +short api.yourdomain.com      # must print your VM's public IP
```

---

## 11. Open both firewalls

### 11a. The cloud rule (Oracle Console)

Menu → **Networking** → **Virtual Cloud Networks** → your VCN → **Subnets** → your
subnet → **Security Lists** → the default list → **Add Ingress Rules**.

Add two, both with **Source CIDR `0.0.0.0/0`**, **IP Protocol TCP**:

| Destination Port | Why |
| --- | --- |
| 80 | Let's Encrypt's challenge, and the redirect to HTTPS |
| 443 | the API itself |

Leave *Stateless* unchecked.

### 11b. The host rule (on the VM) — the step people miss

Oracle's images ship with a local firewall that blocks everything except SSH, on
top of the cloud rule. Opening only the cloud rule leaves you with a connection
that hangs and no error message.

**[VM] — Ubuntu images**

```bash
sudo iptables -I INPUT 1 -p tcp --dport 80  -j ACCEPT
sudo iptables -I INPUT 1 -p tcp --dport 443 -j ACCEPT
sudo netfilter-persistent save
```

**[VM] — Oracle Linux images**

```bash
sudo firewall-cmd --permanent --add-service=http --add-service=https
sudo firewall-cmd --reload
```

**[MAC] — check from outside** (nothing is listening yet, so "connection refused"
is the *good* answer here; a hang or timeout means a firewall is still shut):

```bash
nc -vz api.yourdomain.com 443
```

---

## 12. Certificate, then nginx

Getting the certificate first avoids a chicken-and-egg: the config in this repo
names certificate files, and nginx will not start if they do not exist yet.

**[VM]**

```bash
sudo apt install -y nginx certbot          # Ubuntu
# sudo dnf install -y nginx certbot        # Oracle Linux

sudo systemctl stop nginx

sudo certbot certonly --standalone \
  -d api.yourdomain.com \
  --agree-tos -m you@example.com --non-interactive \
  --pre-hook "systemctl stop nginx" --post-hook "systemctl start nginx"
```

Those hooks are saved into the renewal config, so the automatic 90-day renewal
stops nginx, renews, and starts it again without you doing anything.

**[VM] — make a token and install the config**

```bash
openssl rand -hex 32          # copy this, it is your API key
```

```bash
sudo cp /opt/firefly/server/deploy/nginx.conf /etc/nginx/sites-available/firefly-api
sudo nano /etc/nginx/sites-available/firefly-api
```

Change exactly three things:

1. `REPLACE_WITH_TOKEN` → the string `openssl` just printed.
2. Every `api.example.com` → your real domain (four places).
3. Both `/etc/letsencrypt/live/api.example.com/` paths → your real domain.

Then enable it and start:

```bash
sudo ln -sf /etc/nginx/sites-available/firefly-api /etc/nginx/sites-enabled/
sudo rm -f /etc/nginx/sites-enabled/default
sudo nginx -t
sudo systemctl restart nginx
```

`nginx -t` prints `syntax is ok / test is successful` before you restart. If it
complains about a missing certificate file, the paths in step 3 do not match what
certbot created — check with `sudo ls /etc/letsencrypt/live/`.

---

## 13. Test from your Mac

```bash
TOKEN=<the token you generated>

# no token -> 401
curl -s -o /dev/null -w '%{http_code}\n' https://api.yourdomain.com/health

# with token -> 200 and the record count
curl -s -H "Authorization: Bearer $TOKEN" https://api.yourdomain.com/health

# a real query
curl -s -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
  -X POST https://api.yourdomain.com/strategy \
  -d '{"actions":[2,0,0,0,0,0,1],"board":"As7h2d"}' \
  | python3 -m json.tool | head -40
```

Done.

---

## 14. Running it day to day: closing the terminal, stopping, restarting

### Can I close the terminal?

It depends which of three things you started.

| How you started it | Survives closing the terminal? | Survives a reboot? |
| --- | --- | --- |
| `./build/server --port 8080 &` — the §8 smoke test | Not reliably | No |
| `sudo systemctl enable --now firefly-api` — §9 | **Yes** | **Yes** |
| `ssh -N -L 8080:...` — the tunnel, on your Mac | No, but see below | n/a |
| `ssh -f -N -L 8080:...` — backgrounded, §9b | Yes | No |
| `npm run dev` — the front end, on your Mac | No | No |

The `&` in section 8 only backgrounds the process **inside your shell**. When SSH
disconnects, the terminal it was attached to disappears, and whether the process
outlives that is down to shell settings you should not have to think about. It is
a smoke test, not a way to run a service.

`systemctl enable --now` is the real answer. systemd starts the server as a
service owned by the operating system, with no connection to your login session
at all. You can close the terminal, close the laptop, fly somewhere — it keeps
serving, and it comes back by itself after a reboot or a crash.

**If you are using the SSH tunnel:** closing that terminal does **not** stop the
API. The tunnel is just a private road from your Mac to the VM. The service keeps
running; you simply cannot reach it from your Mac until you open the tunnel again
with the same `ssh -N -L` command.

### Checking on it when you come back

```bash
systemctl is-active firefly-api      # "active"
systemctl is-enabled firefly-api     # "enabled" = it will start at boot
systemctl status firefly-api --no-pager
curl -s localhost:8080/health
```

### Stopping it

There are two independent switches: **running right now**, and **starts at boot**.

```bash
sudo systemctl stop firefly-api            # stop now; still starts at next boot
sudo systemctl disable firefly-api         # keep running now; do not start at boot
sudo systemctl disable --now firefly-api   # both: stop now and stay stopped
```

And to bring it back:

```bash
sudo systemctl start firefly-api
sudo systemctl enable --now firefly-api    # start now and at every boot
sudo systemctl restart firefly-api         # after a rebuild or a new strategy file
```

**Do not stop it with `kill`.** The unit sets `Restart=always`, so systemd notices
the process die and starts a fresh one two seconds later — it will look like the
kill did nothing. `systemctl stop` is an explicit instruction, and the restart
policy does not override it.

(`kill %1` *is* the right way to stop the §8 smoke-test copy, because that one is
not under systemd.)

### nginx is a separate service

```bash
sudo systemctl stop nginx        # public endpoint down, the API itself still runs
sudo systemctl start nginx
```

Stop only the API and nginx stays up answering `502 Bad Gateway`. Stop only nginx
and the API keeps running but is unreachable from outside. To take the whole thing
down:

```bash
sudo systemctl stop nginx firefly-api
```

Stopping the API releases its ~312 MB of resident memory and drops the strategy
mappings from the page cache. Starting it again re-reads the tables, which takes a
few seconds — the `--warm` flag in the unit means it faults the mappings back in
up front rather than making the first few requests pay for it.

---

## 15. The front end: starting it, stopping it, pointing it somewhere

`web/` is the browser client — a keyboard-driven range viewer. It is a **static
site**: it builds to plain files and does no work of its own, so it never runs on
the VM unless you choose to serve it there. Its own reference is
[web/README.md](../web/README.md); this section is only about running it.

It needs **Node 20.19+ or 22.12+** (Vite 7's requirement), on whichever machine shows
the browser:

```bash
brew install node      # [MAC], once
cd ~/Poker/Firefly/web
npm install            # once, and after any dependency change
```

### 15a. Starting it

```bash
cd ~/Poker/Firefly/web
npm run dev
```

That prints `http://localhost:5173/` and stays in the foreground. **It is not the
API.** The dev server forwards every `/api` request to `127.0.0.1:8080`, so one of
the two things below has to be true before the page shows anything:

| You want to read | What must be running |
| --- | --- |
| the strategy on your Mac | `./build/server --port 8080` from the repository root |
| the strategy on the VM | the SSH tunnel from §9, `-L 8080:127.0.0.1:8080` |

Nothing about the front end changes between those two — the tunnel makes the VM
look like localhost, which is the whole point of it. Start them in this order:

```
firefly-api on the VM   ->   ssh -N firefly   ->   npm run dev   ->   open localhost:5173
```

If the page loads but the left column reads `Failed to fetch`, the tunnel or the
local server is down, not the front end. Check with `curl -s localhost:8080/health`
before touching anything in `web/`.

### 15b. Stopping it

Ctrl-C in its terminal. If you lost the terminal:

```bash
lsof -ti:5173 -sTCP:LISTEN | xargs kill
```

`-sTCP:LISTEN` is doing real work there. Plain `lsof -ti:5173` also lists every
process holding a **connection** to that port, which includes the browser tab you
have open — piping that into `kill` closes your browser along with the dev server.

Stopping the front end has no effect on the API or the tunnel; they are three
independent processes and each is stopped on its own.

```bash
lsof -ti:5173 -sTCP:LISTEN | xargs kill    # front end, on your Mac
pkill -f "8080:127.0.0.1:8080"             # tunnel, on your Mac
sudo systemctl stop firefly-api            # the API, on the VM
```

### 15c. Building it for real

`npm run dev` is a development server: it recompiles on every keystroke and is slower
than the built site. For a fixed copy:

```bash
npm run build      # writes web/dist/, a few hundred KB of static files
npm run preview    # serves that build on http://localhost:4173/
```

`preview` proxies `/api` exactly as `dev` does, so it still wants port 8080 open.
`dist/` is gitignored and is safe to delete; `npm run build` recreates it.

### 15d. Serving it from the VM instead

Only worth doing if you want the viewer reachable from a machine that cannot open an
SSH tunnel. It requires the public HTTPS setup from §10–§12 first.

Copy the build up and let nginx serve it beside the API:

```bash
# [MAC]
npm run build
rsync -avP dist/ ubuntu@<VM_PUBLIC_IP>:/opt/firefly/web/dist/
```

Add to the `443` server block in `/etc/nginx/sites-available/firefly-api`, **above**
the existing `location /`:

```nginx
location = / { root /opt/firefly/web/dist; try_files /index.html =404; }
location /assets/ { root /opt/firefly/web/dist; }
```

Served this way the page and the API share an origin, so
`VITE_API_URL` stays unset, the built-in `/api` path works, and **no CORS
configuration is needed** — leave the `$cors_origin` map in `nginx.conf` empty. The
`$cors_origin` map only matters if the page is hosted somewhere else entirely.

> **The token problem does not go away.** nginx still demands
> `Authorization: Bearer ...` on `/api`, and the only way a static page can send that
> is to carry it in its bundle, where any visitor can read it. If the viewer must be
> public, put HTTP Basic auth on the nginx location instead and let the browser
> prompt for it — the credential then lives in the browser's keychain rather than in
> a file you serve to strangers. The SSH tunnel avoids the question entirely, which
> is why §9 is still the recommended route.

---

## 16. Updating later

**Code changed on GitHub:**

```bash
cd /opt/firefly
git pull
g++ -std=c++2b -O2 -I. -pthread -o build/server server/server.cpp -lz
sudo systemctl restart firefly-api
```

**A new strategy file from a new solver run** — from the Mac, then the VM:

```bash
# [MAC]
rsync -avP solver/strategies/strategy_74011.bin ubuntu@<IP>:/opt/firefly/solver/strategies/

# [VM]
cd /opt/firefly
./build/indexBuilder                    # mandatory: the old index no longer matches
sudo systemctl restart firefly-api
```

If the abstraction changes (`METAS_BASIC` / `MEMORY_BASIC` in `config.h`), the spec
string changes with it — `74011` becomes something else — and you need the
matching `strategy_<spec>.bin`, a rebuilt server, and a fresh index.

**The front end**, which needs nothing on the VM unless you are serving it there
(§15d):

```bash
# [MAC]
cd ~/Poker/Firefly/web
git pull && npm install       # npm install only when package.json moved
```

`npm run dev` picks up source changes as you save them, so there is nothing to
restart. A new strategy file needs no front-end change at all — it reads whatever the
API serves.

---

## 17. When something is wrong

| Symptom | Cause | Fix |
| --- | --- | --- |
| `cannot find -lz` at link time | zlib headers missing | `sudo apt install zlib1g-dev` |
| `solver/handAnalysis/boardClasses.bin has an unexpected size` | copy truncated or missing | re-run the rsync; compare against the byte table in §5 |
| `... is not in the 28 byte record format` | strategy file in the old 44-byte double layout, or a partial copy | re-copy `strategy_74011.bin` from a run of the current solver |
| `... Rebuild the index.` | index and strategy disagree | `./build/indexBuilder` |
| `failed to open ... for reading` | wrong working directory | run from `/opt/firefly`; check `WorkingDirectory` in the unit |
| Service dies instantly | see the real error | `journalctl -u firefly-api -n 50 --no-pager` |
| `curl` from the Mac hangs, no error | host firewall still closed | §11b |
| `curl` says connection refused | nginx not running | `systemctl status nginx`, `sudo nginx -t` |
| 401 with a token you believe is right | token mismatch | the nginx `map` matches the *whole* header, so it must be exactly `Bearer <token>` |
| 502 Bad Gateway | nginx is up, the API is not | `systemctl status firefly-api` |
| certbot fails | DNS or port 80 | `dig +short api.yourdomain.com`, and check §11 |
| First request after a restart is slow | mappings not faulted in | already handled: `--warm` is in the unit's `ExecStart` |

Tunnel and front end:

| Symptom | Cause | Fix |
| --- | --- | --- |
| `curl localhost:8080` connection **refused** | no tunnel, nothing listening | reopen it, §9b |
| `curl localhost:8080` connection **reset** | tunnel is up, the API is not | `sudo systemctl status firefly-api` on the VM |
| `curl localhost:8080` hangs | SSH died without closing the socket | `pkill -f "8080:127.0.0.1:8080"`, then reopen |
| Tunnel dies whenever the laptop sleeps | no keepalives | `ServerAliveInterval` in `~/.ssh/config`, §9c |
| `bind: Address already in use` on `ssh -L` | a tunnel is already open | it is working; or move the local port, §9a |
| Tunnel connects but every request fails | `-R` used where `-L` was meant | §9a |
| Page loads, left column says `Failed to fetch` | port 8080 not reachable | `curl -s localhost:8080/health` first |
| `npm run dev` — `EADDRINUSE 5173` | a dev server is already running | `lsof -ti:5173 -sTCP:LISTEN \| xargs kill` |
| Killing the dev server closed the browser | `lsof -ti:5173` matched the browser's connection | add `-sTCP:LISTEN`, §15b |
| Page is blank, console shows a CORS error | served from another origin than the API | §15d — same-origin serving, or name the origin in `$cors_origin` |
| Two `400`s per hand in the network log | how a street rollover is announced | expected; see web/README.md |

Logs:

```bash
journalctl -u firefly-api -f      # the API
sudo tail -f /var/log/nginx/error.log
```

---

## 18. What it costs you on the box

| | |
| --- | --- |
| Resident memory | ~312 MB — the board-class table and the two hand-meta maps. Fixed; does not grow with the strategy. |
| File-backed mappings | ~750 MB — the strategy and its index. Reclaimable page cache, not RSS. Grows with the strategy. |
| Disk | ~910 MB of data plus a 1.5 MB checkout |
| Latency | ~2 ms per query |
| Throughput | ~580 requests/second measured across 8 concurrent clients |

Comfortable on 12 GB, with room for the strategy file to grow several times over
before anything needs rethinking.
