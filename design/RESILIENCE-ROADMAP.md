# Resilience roadmap — the cooperative loop (post-b61)

## The principle (what the OTConnect hunt taught us)
Any **un-deadlined synchronous call on the WaitNextEvent loop** is a freeze
waiting for the right unlucky moment. Three responses, in order of preference:
**bound it** (deadline/slice), **move it off-loop** (preemptive task / async),
or — when neither is cheap — **instrument it** so the stall self-localizes
instead of presenting as "the app froze." And the meta-lesson: **measure before
fixing.** The b59 audit *guessed* OTOpenEndpoint; the b60 probe *proved*
OTConnect. Diagnostics first, targeted fixes second.

## What's already resilient (Phase 0, shipped)
- **Off-loop decode (b52)** — `cq_decring_pump` runs on a preemptive MP task;
  the PCM ring keeps filling while menus/drags/dialogs freeze the loop
  (`casquinha.c:1617`). Audio survives loop freezes by construction.
- **Async OTConnect (b61)** — the connect issue no longer blocks the loop; a
  server rollout / total backend blackout no longer wedges it (VM-proven:
  105 s freeze → 0).
- **OT-trap probe (b60)** — silent sentinel; names any OT provider trap that
  ever stalls.

## Opportunity register
On-loop = runs on the cooperative loop. Audio-safe = off-loop decode already
protects playback through it (only the UI/network side freezes).

| # | Opportunity | Where | On-loop | Current state | Risk if it bites | Fix cost |
|---|---|---|---|---|---|---|
| O1 | **Loop-limb watchdog** — bracket WNE/PollNetwork/PumpAux/TickCaret/TickView, name any limb over budget | `casquinha.c:2807` | n/a (instrumentation) | none | — (turns unknown stalls into one log line) | Low |
| O2 | **Poll online/offline logging** — connect failures are silent today | `casquinha.c:801` (PumpTx FAILED path) | yes | outages invisible in the log (we saw 0 lines in the 90 s blackout) | — | Trivial |
| O3 | **Prefs ModalDialog starves loop** — nested modal freezes network + ring trim (audio-safe) | `casquinha.c:2440` | yes | known (`:1623`) | Prefs open → polls/UI frozen | Medium |
| O4 | **Cover decode on loop** — `GraphicsImportDraw`, un-deadlined QT | `casquinha.c:885` | yes, audio-safe | foreground, once/album | a bad JPEG/QT hiccup hitches UI | Medium |
| O5 | **Sound Mgr calls** — `SndNewChannel`/`SndPlayDoubleBuffer`/`SndDisposeChannel` | `casquinha.c:1813,1830,1744` | yes | rare (listen start/stop) | listen stalls | Low (instrument) |
| O6 | **Menu/drag/list tracking** — `MenuSelect`/`TrackControl`/`LClick` hold the loop until mouse-up | `casquinha.c:2593,1038,1097` | yes, audio-safe | inherent classic-Mac; ⌘-shortcuts already dodge menu (b30) | UI/net frozen while held | High / accept |
| O7 | **FlushVol per log line** — volume flush on every DbgLog | `casquinha.c:242` | yes | deliberate (tail survives freeze) | slow log volume → per-line loop I/O | Low |
| O8 | **SPSC / interrupt correctness** — ring cursors + b61 `conn_event` notifier write | `casquinha.c:1516`, `cq_transport_ot.c:159` | off-loop | works; b61 added new shared state | subtle race | Low (review) |
| O9 | **UI degraded-state** — show "server unreachable, retrying" on connect-fail | `cq_view` + O2 signals | yes | outage looks like a stale screen | — (UX) | Medium |
| O10 | **Endpoint churn** — fresh open/bind/close per txn; can't pool (gopher closes per response) | `cq_transport_ot.c` | yes | async made each connect cheap | aggregate under storm | Low / defer |

## Ranking (leverage ÷ cost, weighting "compounding / diagnostic-first")
1. **O1 loop watchdog** — compounding; zero risk; tells us which of O3–O7 actually matter (no guessing).
2. **O2 failure logging** — trivial; closes the exact blind spot the blackout exposed.
3. **O3 Prefs modal filter** — a concrete freeze the code already documents; clean idiom.
4. **O9 degraded-state UI** — makes outages legible; pairs with O2.
5. **O8 correctness review** — cheap insurance on the new b61 notifier state.
6. **O7 FlushVol batching** — only if O1 shows it ever stalls.
7. **O4/O5 cover/sound** — O1 covers them; bound only if they bite.
8. **O6 tracking** — accept + document (inherent; audio already protected).
9. **O10 churn** — defer (async removed the sting).

## Phased evolution
- **Phase 1 — Observability (b62).** O1 + O2. Same shape as the b60 probe:
  silent under budget, names the limb when one blows it; log online↔offline
  transitions. *Outcome:* the next unknown stall arrives pre-localized, and we
  learn empirically which Phase-3 items are real. **Recommended first.**
- **Phase 2 — Legibility & the known freeze (b63).** O3 (Prefs services audio +
  polls via a `ModalFilterUPP`, or goes modeless) + O9 (explicit reconnecting
  state). Fixes the two user-facing gaps.
- **Phase 3 — Bound what Phase 1 flagged (b64+).** Whichever of O4/O5/O7 the
  watchdog caught crossing budget, plus O8. Data-driven, not speculative.

## Guardrails carried from the freeze hunt
- Every fix ships behind a flag with a proven fallback (like `CQ_TX_ASYNC_CONNECT`).
- Instrumentation stays silent in the healthy case (threshold-gated).
- Nothing off-loop touches the Toolbox / Memory Manager / OT (MP-task discipline).
- Validate on the VM against the real failure mode; keep the sink evidence.
