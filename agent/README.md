# pd-agent — the semantic print server

The local printer agent of [docs/techspec.md §5](../docs/techspec.md), built from the
same C++ core every wrapper binds. It is the **shared agent** deployment shape of
[docs/sdk-spec.md §12](../docs/sdk-spec.md): one process near the printers, a small
submission API in front of it, and one journal behind it.

It exists because of a single invariant.

## The single-owner invariant

> **Each printer has exactly one connection owner at a time.**
> ([docs/sdk-spec.md §14](../docs/sdk-spec.md))

Two independent facts depend on it:

- **Attribution.** A `GS ( H` completion echo is only attributable on the socket that
  sent the marker. Two writers on one printer and the echoes misroute — the receipt that
  "confirmed" may be somebody else's.
- **Deduplication.** Idempotency keys only dedupe *within one journal*. Two tills each
  running their own queue against one kitchen printer have two journals, which cannot
  dedupe each other — and the duplicate-ticket problem returns one layer up, which is
  the problem this SDK exists to remove.

So `pd-agent` holds exactly **one `PrinterDriver`**: one job store, one key index, one
instance nonce. The same order key submitted by two tills over two HTTP connections
prints **once**, and the second submission gets the first job's evidence back.

**Do not** point a second agent, a till running its own driver, CUPS, or a raw
port-9100 client at a printer this agent owns. The agent refuses to open two lanes onto
one `host:port` itself, and reports `foreignWriterDetected` when it sees a completion
echo carrying a token it never issued — but nothing can *stop* another host from writing
to the printer. That is a deployment rule, not a runtime guarantee.

Rule of thumb: any printer whose duplicates are expensive (kitchen, bar) belongs behind
the agent. A printer used by exactly one device (its own receipt printer) can be owned
directly by that device.

## Build and run

```sh
cmake -S . -B build && cmake --build build --target pd-agent
./build/pd-agent --store /var/lib/pd-agent --bind 0.0.0.0 --port 8080 \
                 --printer kitchen=192.168.1.101,profile=xprinter_s_series,width=576
```

Flags:

| Flag | Meaning |
|---|---|
| `--config <file>` | JSON config; command-line flags win over it |
| `--store <dir>` | job journal directory. **Without it the agent runs in memory**: no crash recovery, never right for kitchen tickets |
| `--bind <addr>` | IPv4 listen address (default `127.0.0.1`) |
| `--port <n>` | listen port (default `8080`; `0` picks a free one) |
| `--workers <n>` | connection workers (default 4). Accept is always one thread; the workers keep a job waiting on a fence from blocking `/healthz` |
| `--wait <ms>` | how long `POST /jobs` waits for a terminal state before answering `202` (default 20000) |
| `--printer <id>=<host>[:port][,width=<dots>][,profile=<name>]` | add a printer without a config file; repeatable |

`--profile` names an entry from the device database (`pdctl print --profile list`), or
`xp-s260m` / `generic` for the two built-in engine profiles.

### Config file

```json
{
  "bind": "0.0.0.0",
  "port": 8080,
  "store": "/var/lib/pd-agent",
  "workers": 4,
  "waitMs": 20000,
  "printers": [
    { "id": "kitchen",
      "tcp": { "host": "192.168.1.101", "port": 9100, "connectTimeoutMs": 3000 },
      "widthDots": 576,
      "profile": "xprinter_s_series" },
    { "id": "bar",
      "tcp": { "host": "192.168.1.102" },
      "widthDots": 384,
      "profile": "xprinter_pos58" }
  ],
  "cloudprnt": [
    { "id": "counter",
      "mac": "00:11:62:aa:bb:cc",
      "mediaTypes": ["application/vnd.star.line"],
      "maxPendingJobs": 32 }
  ]
}
```

`cloudprnt` printers have no host and no port — see below.

## Routes

| Route | Purpose |
|---|---|
| `POST /jobs` | submit a receipt; answers with the evidence document |
| `GET /jobs/<id\|key\|token>` | the same document for a job already known |
| `GET /printers` | every printer this agent owns |
| `POST /printers` | add one at runtime |
| `GET /printers/<id>/status` | device status; `?refresh=1` queries the device |
| `GET /healthz` | liveness, journal depth, instance nonce |
| `POST\|GET\|DELETE /cloudprnt/<id>` | the CloudPRNT printer's own poll, job download and confirmation |
| `POST /cloudprnt/<id>/jobs` | hand bytes to a CloudPRNT printer |
| `GET /cloudprnt/<id>/jobs[/<token>]` | what is queued, and how each job ended |
| `GET /cloudprnt` | every polling printer, with its last poll |

Path segments are percent-decoded **after** the path is split, so an idempotency key
containing `#` and a base-94 verification token containing `/` both survive — but the
client must URL-encode them (`order-7F3A#kitchen-1` → `order-7F3A%23kitchen-1`).

### Submit a receipt

```sh
curl -sS localhost:8080/jobs -H 'content-type: application/json' -d '{
  "printerId": "kitchen",
  "key": "order-7F3A-92C1#kitchen-1",
  "payload": { "text": "ORDER 7F3A-92C1\nTable 4\n2x Pilsner\n1x Goulash" },
  "options": { "cut": "partial", "timeoutMs": 8000 }
}'
```

Three payload tiers, exactly the ones in [docs/api.md](../docs/api.md) §3:

```jsonc
"payload": { "text": "plain lines, encoded in the profile's code page" }

"payload": { "rasterBase64": "<8-bit grayscale, row-major, 0 = black>",
             "width": 576, "height": 240 }

"payload": { "dslTemplate": { "v": 1, "template": true, "blocks": [ … ] },
             "model":      { "order": { "id": "7F3A-92C1" } } }
```

The DSL tier binds and renders in the core ([docs/receipt-dsl.md](../docs/receipt-dsl.md)),
so every platform produces identical paper. Its render report travels back on the
response as `render[]` — every degradation declared, nothing silent. The document's own
`meta.cut` and `meta.margins` are applied unless `options` overrides them; the profile's
blade-clearance floor is unconditional either way.

`options` accepts `cut` (`profile|partial|full|none`), `openDrawer`, `preflight`
(`strict|skip`), `timeoutMs`, `topFeedDots`, `bottomFeedDots`, `verificationId` and
`waitMs`.

### The evidence document

```json
{
  "job": "0f2c…",
  "state": "DoneSoftware",
  "evidence": {
    "transportAccepted": true,
    "printFence": "GS(H) fn48",
    "fenceResponse": true,
    "cutterStatus": "clear",
    "paperStatus": "ok"
  },
  "grade": "A",
  "authority": "PhysicalPrinter",
  "method": "GS(H) fn48",
  "token": "7fK2",
  "key": "order-7F3A-92C1#kitchen-1",
  "printerId": "kitchen",
  "outcome": "Done",
  "confidence": "CutFaultFree",
  "attempt": 1,
  "terminal": true,
  "deduped": false
}
```

Read it as a claim plus the evidence behind the claim:

- `transportAccepted` — bytes left this host. False on `FailedKnown`, which is what
  makes `FailedKnown` safe to retry.
- `printFence` / `fenceResponse` — which ordered fence this printer uses, and whether it
  answered. A printer with no fence still reaches `DoneSoftware`; it just does so on
  grade **E**, and `fenceResponse` stays false. The agent never upgrades a claim.
- `cutterStatus` / `paperStatus` — the device's own status, never inferred from the fact
  that a cut command was sent. A processed cut is not a fault-free cut
  ([techspec §3.1](../docs/techspec.md)).
- `grade` — the hierarchy of
  [compatibility-brief §24](../docs/compatibility-brief.md): **A** explicit device
  completion, **B** ordered status fence, **C** device status around the transmission,
  **D** spooler/server, **E** transport only.
- `token` — the `GS ( H` verification identifier printed on the paper as `V:`. Hand it
  back on `GET /jobs/<token>` and the journal names the job that paper came from.

Status codes: **201** a new job that reached a terminal state, **202** submitted and
still running (poll `GET /jobs/<key>`), **200** an existing key — `deduped: true`, and
nothing printed. `202` is never a failure and never a cue to resubmit.

`outcome` is deliberately tri-state: `Done`, `Failed`, `Unknown`. There is no success
boolean, because collapsing `Unknown` into either bucket is the bug that produces
duplicate kitchen tickets. **The agent never retries out of `Unknown`** — that is an
operator decision ([techspec §7](../docs/techspec.md)).

### Look a job up

```sh
curl -sS localhost:8080/jobs/order-7F3A-92C1%23kitchen-1   # by key
curl -sS localhost:8080/jobs/0f2c9a1e…                     # by job id
curl -sS localhost:8080/jobs/7fK2                          # by the V: code on the paper
```

### Printers

```sh
curl -sS localhost:8080/printers
curl -sS 'localhost:8080/printers/kitchen/status?refresh=1'
curl -sS localhost:8080/printers -H 'content-type: application/json' -d '{
  "id": "bar", "tcp": { "host": "192.168.1.102", "port": 9100 },
  "widthDots": 384, "profile": "xprinter_pos58"
}'
```

Adding a printer whose id is taken, or whose `host:port` another lane already owns,
answers **409** — the single-owner invariant, enforced inside the process.

### CloudPRNT printers

A CloudPRNT printer is the opposite topology: **it** polls **us**. Nothing here can open a
socket to it, so it is not a `PrinterDriver` lane, has no `tcp` block, and never touches
the job journal — there is no send for a journal to record. Point the printer's CloudPRNT
URL at `http://<agent>:8080/cloudprnt/<id>` and it will do the rest
([wire-protocols §2](../docs/wire-protocols.md)):

```sh
# hand it a document — already encoded, because nothing here renders for a puller
curl -sS localhost:8080/cloudprnt/counter/jobs \
     -H 'content-type: application/vnd.star.line' --data-binary @receipt.bin
# {"token":"cp-…","state":"queued","grade":"E","method":"none", …}

curl -sS localhost:8080/cloudprnt/counter/jobs/cp-…
# after the printer confirms:
# {"state":"confirmed","outcome":"Done","code":200,"grade":"A",
#  "authority":"PhysicalPrinter","method":"CloudPRNT","confidence":"PrintConfirmed"}
```

JSON works too: `{"mediaType": "...", "base64": "..."}` or `{"text": "..."}`.

The three rules the server keeps, which are what make this path safe:

- **Retained until confirmed.** The `DELETE` is the only thing that deletes a job. A
  printer that lost power mid-transfer polls again and gets the same token.
- **Idempotent download.** `GET` never consumes: the same token yields the same bytes as
  often as the printer asks, and a repeated `DELETE` answers success rather than sending a
  retrying printer round the loop forever.
- **Keyed by identity + token.** A job belongs to one printer. Pin `mac` in the config and
  a device claiming the route with another MAC is answered as though the route did not
  exist; leave it out and the first MAC to poll is adopted. Refusals are counted as
  `identityRefusals` — the polling shape of `foreignWriterDetected`.

Grading is the same hierarchy as everywhere else
([compatibility-brief §24](../docs/compatibility-brief.md)): confirmation `code=200` is a
job-level statement by the printer, so **A** / `PhysicalPrinter` / `CloudPRNT`. Everything
else is not that. The documented failure codes come back as honest failures —
`410 → PreflightPaperOut`, `420 → PreflightCoverOpen`, `411`/`412 →
PreflightHardwareError`, `510`/`511`/`512`/`521 → Unsupported`, `520 →
TimeoutAwaitingCompletion` — and a job that was downloaded but never confirmed has **no
outcome at all** until it is, because a poll only proves the printer is alive. The status
codes that describe the device (`211` paper low, `410` paper out, `420` cover open, `411`
jam) surface as the same `DeviceEvent` values an ASB frame produces; the ones the enum has
no member for (`201`, `220`, `230`, `231` …) are recorded verbatim under `conditions`
rather than bent onto a neighbouring event.

`GET /printers/<id>` answers for a polling printer too, from its last poll. `?refresh=1`
cannot be honoured there — there is no socket to send `DLE EOT` down — and the response
says `refreshSupported: false` instead of pretending the snapshot is fresh.

### Health

```sh
curl -sS localhost:8080/healthz
# {"ok":true,"printers":2,"jobs":184,"store":"/var/lib/pd-agent","durable":true,
#  "instanceNonce":"7f","recoveredJobs":0,"foreignWriterDetected":false,"uptimeMs":91234}
```

`recoveredJobs` counts jobs that were in flight when the process last stopped and were
reclassified on load — `Unknown`, awaiting an operator, never auto-reprinted.
`foreignWriterDetected` is sticky: once something else has been seen writing to a
printer this agent owns, the deployment is wrong until someone fixes it.

## Running it as a service

```ini
# /etc/systemd/system/pd-agent.service
[Unit]
Description=pd-agent receipt printing agent
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/pd-agent --config /etc/pd-agent/config.json
User=pd-agent
Group=pd-agent
StateDirectory=pd-agent
Restart=always
RestartSec=2

# The agent needs a journal directory and a socket. Nothing else.
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths=/var/lib/pd-agent

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now pd-agent
journalctl -u pd-agent -f
```

`Restart=always` is safe: a job that was in flight when the process died comes back as
`Unknown` rather than being reprinted.

## Operational notes

- **No TLS and no authentication.** Bind loopback, or a trusted LAN segment — the same
  segment the port-9100 printers are already on. Anything exposed beyond that belongs
  behind a reverse proxy that terminates TLS and authenticates.
- **`--store` is not optional in production.** Without it there is no journal, so a
  restart forgets every key and the fleet-wide dedupe that justifies the agent is gone.
- **The agent is not a queue.** Store-and-forward, TTLs and priority are the print-queue
  addon's policy ([sdk-spec §12](../docs/sdk-spec.md)); the agent submits through the
  same core path a direct caller would and reports what came back.
- **Bodies are `Content-Length` only.** `Transfer-Encoding: chunked` is answered 501.
- **Backpressure is loud.** Beyond 64 accepted-but-unhandled connections the agent
  answers 503 and closes, rather than accumulating sockets toward a stalled printer.
- **Finding printers**: `pdctl discover 192.168.1.0/24` sweeps a subnet using only the
  non-printing `DLE EOT 1` probe, then `pdctl probe <address>` identifies one device.
