# Static audit — 120 s cooperative-loop freeze (b59)

Static source audit only. No VM, no runtime log, no k8s cluster, no Inside Macintosh /
Open Transport reference were available. Every claim is tagged:
`[CONFIRMED-CODE]` (quoted proof), `[INFERRED-CODE]` (reasoning shown),
`[HYPOTHESIS]`, `[CANNOT-VERIFY-STATICALLY]`.

---

## 1. Repo state

- HEAD = `add154e89cc3c090e764c0a3fca5a35b53d55313` — matches the expected `add154e` ("b59"). `[CONFIRMED-CODE]`
- `git status`: "nothing to commit, working tree clean". `[CONFIRMED-CODE]`
- Files audited: `os9/casquinha.c` (2813 lines), `src/cq_transport_ot.c` (463), `src/cq_transport.h` (109).

## 2. Premises taken as given (NOT re-derived)

- Cooperative `WaitNextEvent` loop stopped iterating ~120.3 s; loop heartbeat and `/now` poll both dark.
- Preemptive MP3 decode MPTask (~23k iters) and the Sound double-back interrupt kept firing — only the cooperative loop wedged.
- socket→ring drain barely advanced during the wedge, then flushed ~1.1 MB on recovery; recovery coincided with a track change.
- Timeline correlates with a rollout of the gopher-server pod (10.0.100.112:70) landing dead-center in the window. The Icecast audio pod (10.0.100.113:8000) did NOT restart.
- Hosts are dotted-quad IPs.

## 3. Re-verified eliminations (each cited)

**Host literals.**
- `CQ_DEFAULT_HOST "10.0.100.112"` — `os9/casquinha.c:82`; `gHost[64] = CQ_DEFAULT_HOST` at `os9/casquinha.c:94`. `[CONFIRMED-CODE]`
- `10.0.100.113` appears in NO source file (`grep -rn` over `os9/`, `src/` → no match). The stream host is parsed from the `.pls` body: `ToggleListen` fetches `/spot/stream.pls` (`os9/casquinha.c:2146`) → `cq_pls_first_url` → `StartStream(url)` → `ParseHttpUrl` (`os9/casquinha.c:2118-2130`). Confirms the stream host is data, not a code literal. `[CONFIRMED-CODE]`

**DNS path is unreachable for IP hosts.** `resolve_host` short-circuits dotted-quads before any OT resolver call:
```
if (parse_dotted_quad(host, out)) return 1;                    // src/cq_transport_ot.c:164
isv = OTOpenInternetServices(...);                             // :166  (only past the return)
err = OTInetStringToAddress(isv, (char *)host, &info);         // :168  blocking DNS
```
Both the gopher host (`10.0.100.112`) and the stream host (`10.0.100.113`, per premise) are dotted-quads, so `parse_dotted_quad` (`:137-153`) returns 1 and `OTOpenInternetServices`/`OTInetStringToAddress` are never reached. The blocking DNS call is out of the picture. `[CONFIRMED-CODE]`

**Endpoint discipline.** Endpoint made synchronous+non-blocking after open, sync-idle events off:
```
OTSetSynchronous(t->ep);  OTSetNonBlocking(t->ep);  OTUseSyncIdleEvents(t->ep, false);   // :190-192
```
`[CONFIRMED-CODE]`

**Deadlines.** `CQ_TX_DEADLINE_SECS 2`, `CQ_TX_WATCHDOG_SECS 5` (`src/cq_transport.h:27-28`); `DEADLINE_TICKS = 2*60 = 120`, `WATCHDOG_TICKS = 5*60 = 300` (`src/cq_transport_ot.c:34-36`). The connect deadline is checked ONLY in `ST_CONNECTING`, the watchdog only while `status==RUNNING` and disarmed for a streaming receive:
```
if (t->st == ST_CONNECTING && (now - t->connect_tick) > DEADLINE_TICKS) ...     // :330
if (!(t->streaming && t->st == ST_RECEIVING) && (now - t->start_tick) > WATCHDOG_TICKS) ...  // :332-333
```
`[CONFIRMED-CODE]` — note the scope limit (see §7).

**ComputeSleep max = 60 ticks (1 s).** `return gTx ? 1L : ((gSuspended && gAuSt == AU_IDLE) ? 60L : 10L);` (`os9/casquinha.c:2615`). Max return is `60L`. Cannot itself cause a 120 s freeze. `[CONFIRMED-CODE]`

**ParkDecTask bounded, 30-tick fallback.** `if ((unsigned long)TickCount() - t0 > 30) { ...gMPDecode=0; return; }` inside `while (gDecAck != CQ_DEC_PARK)` (`os9/casquinha.c:1704-1711`). Bounded to ~30 ticks (~0.5 s) then inline fallback. `[CONFIRMED-CODE]`

**DrainStreamToDec bounded.** `for(;;)` breaks on `!contig` (comp-ring backpressure) or `!n` (nothing drained) — `os9/casquinha.c:1835-1848`. `cq_tx_drain` is a plain buffer memmove (`src/cq_transport_ot.c:346-355`), no I/O. Bounded by ring capacity per pass. `[CONFIRMED-CODE]`

**AU_SYNC staging drain bounded.** `while (done < gCompLen)` breaks on `!contig`; `gCompLen` is ≤ the 64 KB staging (`os9/casquinha.c:1929-1939`). `[CONFIRMED-CODE]`

**DecodeCover bounded input.** Decodes one 64×64 cover via `GraphicsImportDraw` (`os9/casquinha.c:876`) over a bounded JPEG handle; foreground-only, at most once per album per run (`gCovers` cache, `os9/casquinha.c:915-917`). `GraphicsImportDraw` is a synchronous QuickTime call not covered by any deadline — see §4. `[CONFIRMED-CODE]` for boundedness of scheduling; runtime duration `[CANNOT-VERIFY-STATICALLY]`.

**PumpTx / PollNetwork non-blocking.** `PumpTx` calls `cq_tx_poll` once (one slice) then, on DONE/FAILED, `AdoptReply` + `cq_tx_free` (`os9/casquinha.c:787-812`). `AdoptReply`'s follow-on actions (`StartFire`/`StartCommand`/`PlayFrom`) only *create* transactions (`cq_tx_new`+`cq_tx_start`) and return — no synchronous drive (`os9/casquinha.c:841-846`, `1309-1315`, `1333-1369`). `[CONFIRMED-CODE]`

## 4. On-loop synchronous calls NOT covered by a TickCount deadline

Every gopher/stream transaction is created with `cq_tx_new`/`cq_tx_stream_new` and driven by `cq_tx_poll` from the loop (`PollNetwork` `os9/casquinha.c:2800`, `PumpAux` `:2801`). `cq_tx_poll`'s `ST_INIT` slice runs `open_and_connect` **fully synchronously**, and completion/teardown run provider close calls — none guarded by the connect deadline or watchdog (those only fire in later states / while RUNNING, §7).

| Call | Site | On loop? | Deadline-governed? | Classification |
|---|---|---|---|---|
| `InitOpenTransport` | `cq_transport_ot.c:76` (via `ot_ensure_up`, once) | yes (first txn) | no | sync one-time init; blocks? `[CANNOT-VERIFY-STATICALLY]` |
| `OTOpenEndpoint` (TCP) | `cq_transport_ot.c:184` | yes | **no** | sync, NULL notifier, issued *before* `OTSetNonBlocking`; churned per txn — see §7/F. Runtime block `[CANNOT-VERIFY-STATICALLY]` |
| `OTBind` | `cq_transport_ot.c:194` | yes | no | after non-blocking set; block `[CANNOT-VERIFY-STATICALLY]` |
| `OTConnect` | `cq_transport_ot.c:204` | yes | subsequent `ST_CONNECTING` wait only | issued sync on a non-blocking ep → returns `kOTNoDataErr`; the *wait* is deadline-bounded, the *issue* is not `[INFERRED-CODE]` |
| `OTRcvOrderlyDisconnect` / `OTSndOrderlyDisconnect` | `cq_transport_ot.c:263,268` | yes | no | per successful txn close `[CONFIRMED-CODE]`; block `[CANNOT-VERIFY-STATICALLY]` |
| `OTRcvDisconnect` | `cq_transport_ot.c:217,277` | yes | no | per refused/reset txn `[CONFIRMED-CODE]` |
| `OTUnbind` + `OTCloseProvider` | `:114, 269-270, 279, 292, 373-374, 383-384` | yes | **no** | provider teardown, run on EVERY txn completion / fail / cancel / free `[CONFIRMED-CODE]`; block `[CANNOT-VERIFY-STATICALLY]` |
| `OTSnd` | `cq_transport_ot.c:236` | yes | watchdog only | non-blocking ep, returns `kOTFlowErr` on full `[CONFIRMED-CODE]` |
| `OTRcv` | `cq_transport_ot.c:254` | yes | watchdog (disarmed while streaming) | non-blocking ep `[CONFIRMED-CODE]` |
| `GraphicsImportDraw` | `os9/casquinha.c:876` | yes (cover decode) | no | synchronous QT; bounded input, ≤1/album/run `[CONFIRMED-CODE]`; duration `[CANNOT-VERIFY-STATICALLY]` |
| `SndNewChannel` / `SndPlayDoubleBuffer` / `SndDisposeChannel` | `os9/casquinha.c:1804,1821,1735` | yes | no | synchronous Sound Mgr, once per listen start/stop `[CONFIRMED-CODE]`; duration `[CANNOT-VERIFY-STATICALLY]` |
| `ModalDialog` (Prefs) | `os9/casquinha.c:2431` | yes | no | **nested modal loop starves the audio ring**, but user-triggered (Edit▸Prefs), not part of the rollout-correlated freeze `[CONFIRMED-CODE]` |
| `MPWaitOnQueue` (quit join) | `os9/casquinha.c:1721,1725` | yes (quit only) | bounded `1000*kDurationMillisecond` ×2 | not reachable during steady-state loop `[CONFIRMED-CODE]` |
| `OTOpenInternetServices` / `OTInetStringToAddress` | `cq_transport_ot.c:166,168` | **no** for IP hosts | n/a | short-circuited by `parse_dotted_quad` (§3) `[CONFIRMED-CODE]` |

**Open/close churn (quantified from constants).** Every transaction opens a fresh endpoint (`open_and_connect`, `:184`) and closes it on completion (`:269-270`), failure (`fail`, `:111-116`), cancel (`:373-374`) or free (`:383-384`) — no endpoint pooling. During the rollout ~5 transaction *types* can be live/retrying: `/now` poll (`gTx`), command (`gCmd`), cover (`gCover`), queue (`gQueueTx`), stream-fact (`gFactTx`), plus fire/pls/search. Retry cadence from constants: `CQ_POLL_TICKS 120` (2 s /now, `:84`), `CQ_QUEUE_POLL_TICKS 600` / `CQ_STREAM_POLL_TICKS 600` (10 s, `:295,157`), backoff cap `CQ_POLL_CAP 1800` (30 s, `:85`). A failing connect burns up to the 2 s connect deadline before `fail`+backoff. Auto-starters are gated to `CQ_MAX_INFLIGHT 3` (`:268`, `TxInFlight` `:1535`), but `gTx` and `gCmd` are NOT gated (`StartCommand` `:841-846`, poll start `:938-942`). So the endpoint open/close rate is on the order of a handful per few seconds early in the storm, tapering toward one-per-30 s per type under backoff — **bounded churn, not thousands/sec** `[INFERRED-CODE]`. Whether that churn exhausts OT/STREAMS resources is `[CANNOT-VERIFY-STATICALLY]`.

## 5. gStream / track-change audit (tasks A–C)

**A. PumpAux and its pumps.** `PumpAux` (`os9/casquinha.c:2156-2337`) advances `gFire`, `gPls`, `gSearchTx`, `gArtistTx`, `gQueueTx`, `gFactTx` each via a single `cq_tx_poll`, plus `ServiceAudio`. Each reply handler parses a bounded buffer (`cq_*_from_response`, `cq_fields_parse`) and frees the txn. **Bounded per pass — modulo the synchronous OT provider calls inside `cq_tx_poll` (§4).** No pump busy-waits, no pump drives another txn to completion synchronously. `[CONFIRMED-CODE]`

**B. StopAudio / gStream (re)open / probe.**
- `StopAudio` (`:1733-1749`): `SndDisposeChannel(...,true)`, `cq_tx_cancel(gStream)` (→ `OTUnbind`/`OTCloseProvider`, `:371-375`), `ParkDecTask` (bounded, §3), buffer resets. No open, no blocking wait beyond the park bound. `[CONFIRMED-CODE]`
- Stream (re)open: `StartStream` (`:2118-2137`) calls `cq_tx_stream_new` + `cq_tx_start` and returns; the stream is thereafter driven in slices by `ServiceAudio`→`cq_tx_poll(gStream)` (`:1860`). No blocking open. `[CONFIRMED-CODE]`
- `OpenAudioOutput` (`:1765-1789`): `NewPtr` allocs, `StartDecTask` (create-once), `ParkDecTask` (bounded), ring init. `StartDoubleBuffer` (`:1793-1826`) issues the synchronous Sound Mgr calls (§4). None deadline-governed but all one-shot per listen. `[CONFIRMED-CODE]`
- No `OpenTptInternet`/`OTOpenInternetServices` on the audio path (IP host, §3). `[CONFIRMED-CODE]`

**C. What a NEW `track_id` from `/now` triggers.** In `AdoptReply`, the only track-change branch:
```
if (tmp.track_id && (!gSnap.track_id || strcmp(tmp.track_id, gSnap.track_id) != 0)) {
    gQueueKick = (unsigned long)TickCount() + 120;      // os9/casquinha.c:721-723
    DbgLog("now: %s - %s", ...);
}
```
It sets a queue re-poll kick and logs. It does **NOT** tear down or re-open audio, does not touch `gStream`/`gChan`, and does not park the decode task on the loop. `[CONFIRMED-CODE]` The end-of-track watchdog (`:696-713`) can call `PlayFrom` → `StartCommand`/`StartFire`, which only *create* a txn (§3). **Conclusion: a track change does no synchronous audio teardown/reopen.** Therefore the "recovery coincided with a track change" correlation is explained by the loop resuming and adopting a fresh `/now` document — the track change is a *symptom of recovery*, not a cause of the freeze, and gives no on-loop block. `[INFERRED-CODE]`

## 6. Unit-mismatch sweep (task E) — result: NOT FOUND

- No `Delay()` call exists anywhere in `os9/casquinha.c` or `src/cq_transport_ot.c` (grep: no match). `[CONFIRMED-CODE]`
- All deadline comparisons use TickCount deltas against tick constants: `> DEADLINE_TICKS` / `> WATCHDOG_TICKS` (`cq_transport_ot.c:330,332`). Correct units. `[CONFIRMED-CODE]`
- Loop-side time offsets are ticks: `gQueueKick = TickCount() + 120` (`:723, 2164`), `ParkDecTask` `> 30` ticks (`:1705`), audio beats `>= 600/300` ticks (`:2012,2047`). Consistent. `[CONFIRMED-CODE]`
- Off-loop waits use `AbsoluteTime`/`kDurationMillisecond`: `MPDelayUntil` with `gAbsPer5Ms` (`:1665`), `MPWaitOnQueue` `1000*kDurationMillisecond` (`:1722`). Correct units, and off-loop anyway. `[CONFIRMED-CODE]`
- The freeze (~120.3 s) is ~60× the 120-tick (2 s) connect/poll constants. I checked specifically for a ticks-as-seconds (60×) confusion: **none exists in code** — every 120 literal is either ticks compared against ticks or a tick offset added to `TickCount()`. The magnitude coincidence is not backed by any code defect. `[CONFIRMED-CODE]` (absence), i.e. do NOT attribute the 120 s to a 60× unit bug.

## 7. Verdict on the OTOpenEndpoint hypothesis (task F)

**Code-structural half — CONFIRMED.**
```
t->ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);   // :184
if (err != kOTNoError ...) { ... }
OTSetSynchronous(t->ep);                                                  // :190
OTSetNonBlocking(t->ep);                                                  // :191
```
- The open is issued with a **NULL notifier** and before any mode is set — it runs **synchronously**, and it precedes `OTSetNonBlocking`. `[CONFIRMED-CODE]`
- The non-blocking discipline structurally **cannot** cover endpoint *open* or *close*: at open the endpoint does not yet exist to carry a blocking mode, and `OTCloseProvider` (`:270` etc.) tears it down. So `OTOpenEndpoint` and `OTCloseProvider` are the loop's synchronous OT surface that no `OTSetNonBlocking` and no TickCount deadline governs. `[INFERRED-CODE]`
- These calls sit directly on the loop path (`cq_tx_poll` ← `PollNetwork`/`PumpAux` ← `main` while-loop, `:2797-2801`) and are churned once per transaction with no pooling (§4). The connect storm is real in structure: unbounded `gTx`/`gCmd` starters + up to 3 gated auto-starters, each opening a fresh endpoint under the rollout's connect failures. `[CONFIRMED-CODE]`

**OT-resource-exhaustion / does-it-block half — CANNOT-VERIFY-STATICALLY.**
Whether `OTOpenEndpoint` (or `OTBind`/`OTConnect`/`OTCloseProvider`/`InitOpenTransport`) actually *stalls* for ~120 s under a server-rollout connect storm — STREAMS/endpoint exhaustion, TIME_WAIT pressure, OT internal serialization — is a runtime property of Open Transport that this repo does not document. Per guardrail 3 it must not be asserted as fact. `[CANNOT-VERIFY-STATICALLY]`

**What would confirm or refute it:** a DbgLog timestamp bracketing the synchronous span — e.g. a log line immediately before `OTOpenEndpoint` (`:184`) and immediately after it returns (and likewise around `OTCloseProvider` `:270`). If one such bracket shows a ~120 s gap while `gDecBeat` keeps advancing, the culprit is that specific trap. The existing per-pass audio heartbeat (`:2031`) already proves the MP task lived through the wedge; it does not localize *which* on-loop call blocked.

## 8. New suspect found?

No new distinct suspect was localizable statically. The audit narrows the on-loop exposure to exactly the set in §4 that is **not** governed by the non-blocking discipline or a TickCount deadline: the synchronous OT provider lifecycle calls (`OTOpenEndpoint`, `OTBind`, `OTUnbind`, `OTCloseProvider`, `InitOpenTransport`) churned per transaction, plus the synchronous QuickTime/Sound Toolbox calls (`GraphicsImportDraw`, `Snd*`). Among these, `OTOpenEndpoint`/`OTCloseProvider` are the strongest structural candidates because they (a) run on every transaction, (b) are un-deadlined, and (c) are provably outside what `OTSetNonBlocking` can cover. `[INFERRED-CODE]`

**Per guardrail 6:** every code path reachable from the WNE loop is bounded EXCEPT the synchronous OT/Toolbox trap calls above, whose *runtime* duration is not statically knowable. So the honest static conclusion is: **the freeze is not statically localizable to a single culprit line; it localizes to the un-deadlined synchronous OT provider open/close surface, and runtime evidence is required to pick the exact trap.**

## 9. Smallest viable fix candidates (each tied to a confirmed finding)

1. **Bracket the un-deadlined OT traps with timestamps before fixing anything** (tied to §4/§7). Cheapest, evidence-first: log `TickCount()` immediately before/after `OTOpenEndpoint` (`:184`) and `OTCloseProvider` (`:270`). This is the single cheapest piece of runtime evidence that decides §7 — do this before any behavioral change.
2. **Endpoint reuse / pooling** (tied to §4 churn + §7): keep one bound TCP endpoint and re-`OTConnect` per transaction instead of open+bind+close every time, to shrink the synchronous provider-call surface exposed to the loop. Only justified if evidence (fix 1) implicates open/close.
3. **Async open with a notifier** (tied to §7 structural gap): pass a notifier to `OTOpenEndpoint` so the open no longer runs synchronously on the loop, moving it under the same poll discipline as connect/send/recv. Larger change; justified only if fix 1 shows the *open* specifically blocks.

**Honest state:** a fix cannot be *selected* without VM repro, because the code-verifiable facts do not tell us which trap (open, bind, close, or init) actually stalls, nor that any does. Fix 1 is the prerequisite; fixes 2–3 are conditional on its result.

## 10. Open questions / required runtime evidence

- Does any of `OTOpenEndpoint` / `OTBind` / `OTConnect` / `OTCloseProvider` / `InitOpenTransport` actually block for the ~120 s span on the loop? (Needs the timestamp bracket, §9.1.) `[CANNOT-VERIFY-STATICALLY]`
- Does the server rollout drive OT/STREAMS resource exhaustion or TIME_WAIT pressure on the client such that a provider call serializes/stalls? (Needs OT reference + VM.) `[CANNOT-VERIFY-STATICALLY]`
- During the wedge, did `gDecBeat` keep advancing (confirming only the loop wedged) — and does the recovery timestamp line up with a specific trap returning? (Needs the runtime log.) `[CANNOT-VERIFY-STATICALLY]`
