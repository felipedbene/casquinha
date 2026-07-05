# CLIENT-PATTERN → Mac OS 9.2 mapping

How each layer of [fhb ▸ CLIENT-PATTERN.md](https://github.com/felipedbene/fhb/blob/main/CLIENT-PATTERN.md)
lands on Classic Mac OS (PowerPC, Open Transport, Toolbox), against DeGelato's
10.5/ppc reference. The four pure layers port verbatim; the glue is a rewrite.

## The seven layers (§1)

| Layer | DeGelato (10.5) | Casquinha (OS 9.2) | Pure? |
|---|---|---|---|
| Transport | `DGGopherClient` — BSD socket + `getaddrinfo` on a worker thread | `cq_transport` — **Open Transport** endpoint, async, advanced from the event loop | I/O only |
| Codec | `DGApiParser` | `cq_codec` — tolerant tokenizer + `FF D8` sniff | ✅ done |
| Model | `DGNowSnapshot`, `DGTrackItem` | `cq_now`, `cq_track` | ✅ done |
| Reconciler | `DGSnapshotGuard`, `DGDebouncer` | `cq_guard`, `cq_debounce` | ✅ done |
| Prefs | `DGServerPrefs` (NSUserDefaults) | `cq_prefs` — a prefs file via `FindFolder` | ✅ later |
| Cache | `DGCoverCache` (dict + disk) | `cq_covercache` — `Handle` map + a Caches folder | ✅ later |
| Controller/View | `DGNowPlayingWindowController` (Cocoa) | `cq_app` + `cq_ui` — Toolbox window + `WaitNextEvent` | glue |
| Shell | `AppDelegate`, `main.m` | `main.c` + `cq.r` (Rez: WIND/MENU/DITL) | glue |

Load-bearing rule unchanged: the **Transport↔Codec seam is where the tests
live**. `cq_codec` never touches Open Transport; `cq_transport` never parses. A
`printf | nc` fixture exercises the whole pure stack offline (it already does —
`make test`).

## The escape-hatch table (§4 — the hard part)

| Fancy / newer-OS path | Boring OS 9 primitive | Why |
|---|---|---|
| CFStream/NSStream, `getaddrinfo` on a thread | **Open Transport** endpoint + `OTInetStringToAddress`, async via the event loop | OT is *the* TCP primitive on 9; a sync call freezes the cooperative machine, so async-with-state-machine is mandatory |
| Worker threads + `performSelectorOnMainThread:` | one `WaitNextEvent` loop; OT notifier sets flags only | no preemptive threads; **no Toolbox from a notifier** (deferred-task ctx) |
| `NSTimer` (2 s poll / 5 s watchdog) | `TickCount()` deltas checked each loop pass (120 / 300 ticks) | the loop is the only clock |
| `NSString` UTF-8 everywhere | UTF-8 in the models; **TEC UTF-8→MacRoman at the `DrawText` boundary only** | QuickDraw is MacRoman; convert late, never in the parser |
| Cocoa programmatic windows/controls | Window / Control / Menu Manager + QuickDraw (+ Appearance Mgr); **Monaco** | no Cocoa; Rez `.r` resources |
| `NSImage` JPEG decode | **QuickTime GraphicsImporter** → `PixMap` (same `FF D8` sniff) | QT decodes JPEG on 9 |
| `NSUserDefaults` / `NSCache` | prefs file + covers folder via `FindFolder` | no defaults db; passive blobs, same shape |
| CoreAudio `AudioQueue` | **QuickTime** MP3 movie from Icecast `:8000` (deferred) | the riskiest piece; a late fio |

## The seven laws (§2) — unchanged, because they're not about the OS

1. **Cancel ≠ un-send** → `cq_debounce` before the OT send. *(pure, done)*
2. **Monotonic ts-guard** vs the 2-replica micro-cache → `cq_guard`, mandatory. *(pure, done)*
3. **A command's reply is an authoritative `/now`** → adopt it through the same guard; no catch-up poll storm.
4. **One hold window**, not per-control → a single scrub/hold on the live-drag sliders (Fio 4).
5. **Poll no faster than the ~1 s micro-cache** → 2 s `TickCount` cadence.
6. **Forward-compatible contract** → ignore unknown keys, tolerate missing, key off `state`. *(pure, done)*
7. **Sniff the one binary endpoint** → `cq_data_is_jpeg` (`FF D8`) before the GraphicsImporter. *(pure, done)*

## Threading discipline (§3) on a machine with no threads

The bridge is on the network, so the wire is slow; the machine cannot preempt.
So the transaction is a **state machine** the event loop advances: open endpoint
→ connect → send `selector\r\n` → drain to `T_ORDREL`/EOF → deliver bytes → parse
→ render. Each pass does a bounded slice and returns to `WaitNextEvent` so the UI
(and the cursor, and every other app) stays live. The watchdog is a `TickCount`
deadline; a LAN transaction that hasn't produced its first data in ~2 s is dead.
Pure message-passing, zero shared mutable state — trivially satisfied when there
is exactly one execution context, *provided* no work happens in an OT notifier.
