# Deploying the strategy API to an Oracle VM

Written to be followed line by line. Every command says which machine to run it
on: **[MAC]** or **[VM]**.

---

## 0. The mental model

Three things have to end up on the VM.

| What | Where it comes from | Size |
| --- | --- | --- |
| Source code | `git clone` from GitHub | 1.5 MB |
| Data tables | copied from your Mac — they are **not** in git | 624 MB |
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
df -h /           # need ~2 GB free; 624 MB of data + 256 MB index + binaries
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

624 MB over a home upload link takes a while — roughly 8 minutes at 10 Mbit/s,
90 seconds at 50 Mbit/s.

**Do not copy** `handScores.bin` (510 MB) or `strategyBase_74011.bin` (93 MB). The
API never reaches showdown and never reads the pruned masks, so both are dead
weight. That is exactly what the `Engine::loadTables(false)` change is for.

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
| `solver/strategies/strategy_74011.bin` | 477107672 |

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
curl -s -X POST localhost:8080/strategy -d '{"actions":[]}' | head -c 400
```

Expect `{"status":"ok", ...}` and then a wall of JSON whose first combo is `AcAd`
raising about 94% of the time. If that works, the hard part is done — everything
left is networking.

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

## 14. Updating later

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

---

## 15. When something is wrong

| Symptom | Cause | Fix |
| --- | --- | --- |
| `cannot find -lz` at link time | zlib headers missing | `sudo apt install zlib1g-dev` |
| `solver/handAnalysis/boardClasses.bin has an unexpected size` | copy truncated or missing | re-run the rsync; compare against the byte table in §5 |
| `... is not in the 44 byte record format` | old or partial strategy file | re-copy `strategy_74011.bin` |
| `... Rebuild the index.` | index and strategy disagree | `./build/indexBuilder` |
| `failed to open ... for reading` | wrong working directory | run from `/opt/firefly`; check `WorkingDirectory` in the unit |
| Service dies instantly | see the real error | `journalctl -u firefly-api -n 50 --no-pager` |
| `curl` from the Mac hangs, no error | host firewall still closed | §11b |
| `curl` says connection refused | nginx not running | `systemctl status nginx`, `sudo nginx -t` |
| 401 with a token you believe is right | token mismatch | the nginx `map` matches the *whole* header, so it must be exactly `Bearer <token>` |
| 502 Bad Gateway | nginx is up, the API is not | `systemctl status firefly-api` |
| certbot fails | DNS or port 80 | `dig +short api.yourdomain.com`, and check §11 |
| First request after a restart is slow | mappings not faulted in | already handled: `--warm` is in the unit's `ExecStart` |

Logs:

```bash
journalctl -u firefly-api -f      # the API
sudo tail -f /var/log/nginx/error.log
```

---

## 16. What it costs you on the box

| | |
| --- | --- |
| Resident memory | ~312 MB — the board-class table and the two hand-meta maps. Fixed; does not grow with the strategy. |
| File-backed mappings | ~750 MB — the strategy and its index. Reclaimable page cache, not RSS. Grows with the strategy. |
| Disk | ~910 MB of data plus a 1.5 MB checkout |
| Latency | ~2 ms per query |
| Throughput | ~580 requests/second measured across 8 concurrent clients |

Comfortable on 12 GB, with room for the strategy file to grow several times over
before anything needs rethinking.
