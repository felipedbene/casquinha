# NOTES — Casquinha

Scratchpad + the load-bearing constraints. The permanent section is **never
remove this**: each line is a scar promoted to a rule per CLIENT-PATTERN.md §5.

## Permanent design constraints

Ported verbatim in spirit from DeGelato (they come from the backend + protocol,
not the OS, so they hold on Mac OS 9 too):

- **Cancel ≠ un-send (R1).** On the LAN the selector is on the wire within one
  event-loop turn, so gopher-spot executes *every* command that reaches the
  Open Transport send; a client-side cancel only stops us listening. Rapid
  transport taps are therefore coalesced **before** the send (`cq_debounce`):
  three fast Next taps skip one track, not three. Idempotent `/now` polls are
  exempt (cancel-and-replace a poll freely).

- **The ts-guard is mandatory — never remove it (R3).** gopher-spot runs two
  load-balanced replicas, each micro-caching `/now` ~1 s, so consecutive polls
  can return `ts` out of order. `cq_guard` drops any snapshot whose `ts`
  regressed; without it the UI flip-flops (track rewinds, seek knob jumps). It
  resets on reconnect so a backend clock-reset can't lock adoption out forever.

New, OS-9-specific (the §4 escape hatches for this era — the R7 analog):

- **Open Transport is async and driven from the `WaitNextEvent` loop — NEVER a
  synchronous OT call.** Classic Mac OS is cooperatively multitasked with no
  preemption, so a blocking OT call freezes the *entire machine*, not just our
  window. The transport is a state machine advanced each pass of the event loop.
  If a notifier is used, it **only sets flags** — it must not call the Toolbox or
  allocate memory (it runs at deferred-task time). This is the OS 9 analog of
  DeGelato's "BSD sockets on a worker + marshal to the main thread," and it is
  stricter, not looser.

- **The event loop is the only clock.** No preemptive threads, no callback
  timers for app logic: the 2 s poll cadence and the ~5 s watchdog are
  `TickCount()` deltas checked each pass (120 / 300 ticks). Poll no faster than
  the server's ~1 s micro-cache (law 5).

- **UTF-8 lives in the models; convert to MacRoman only at the QuickDraw
  boundary.** The wire is UTF-8 (`Construção`); QuickDraw draws MacRoman. Convert
  with the Text Encoding Converter (TEC) right before `DrawText`, never in the
  parser. Accented Latin (ç ã é õ á) round-trips; glyphs MacRoman lacks degrade
  at the draw, not in the data.

- **Detect the one binary endpoint by magic bytes (law 7).** `/cover` is raw
  JPEG on success but a tab-KV *error* document on failure. Sniff `FF D8`
  (`cq_data_is_jpeg`) before handing bytes to the QuickTime GraphicsImporter.

## Status

- **Fio 0 — scaffold.** Repo + Makefile (host test family + Retro68 app family),
  fixtures copied verbatim, docs. Done.
- **Fio 1 — pure core + tests.** `cq_codec / cq_now / cq_track / cq_guard /
  cq_debounce / cq_pls` in plain C99, `make test` green on the modern host
  (97 checks), fixtures wired via `CQ_FIXTURES`. Done — no Retro68/emulator
  needed, exactly as intended.

## Toolchain

Retro68 is built locally (`~/Retro68-build/toolchain`, PowerPC/CFM only:
`--no-68k --no-carbon`). `make app` cross-builds the OS 9 app and drops
`Casquinha.bin`/`.dsk` on the netatalk AFP share for the VM. **UTM is the victory
lap** — the final on-hardware run, not where compile errors get found.

**Interfaces (resolved):** Retro68's open-source Multiversal Interfaces have the
full Toolbox but **no Open Transport / MacTCP / Carbon**. So Apple's **Universal
Interfaces** were folded in (`InterfacesAndLibraries/`, from macintoshgarden's
ready-extracted zip — no MPW `.img` needed) and Retro68 rebuilt
`--universal --skip-thirdparty`. OT now compiles + links. **Link libs for OT**
(order matters): `OpenTransportAppPPC OpenTptInetPPC OpenTptInternetLib
OpenTransportLib` — the App lib gives `InitOpenTransport`, InetPPC gives
`OTOpenInternetServices`, InternetLib gives `OTInetStringToAddress` /
`OTInitInetAddress`.

## Done — the full arc (all fios)

Validated on the VM through Fio 3; Fios 4–7 + audio are **compile+link verified**
with Retro68 and on the share, pending the UTM runtime pass.

- **Fio 3 — live now-playing over Open Transport**, VALIDATED on the VM. Platinum
  window (Appearance Manager, classic PPC — no Carbon): themed background, green
  accent, bold track title (green when playing), Geneva metadata, native
  `DrawThemeTrack` progress bar interpolated between polls. 2 s `TickCount` poll →
  `cq_guard` → render. **TEC UTF-8→MacRoman** at the draw boundary (accents).
  **Graceful 429**: check the `error` key first (never blank a good snapshot),
  `cq_backoff` (pure, tested) eases the poll 2 s→30 s and recovers.
- **Fio 4 — transport controls.** Themed Prev/Play-Pause/Next buttons + volume
  slider + click-to-seek on the bar. Prev/Next coalesced with `cq_debounce`
  (law 1); command reply adopted via the shared guard (law 3); a single hold
  window on the slider (law 4). Commands ride their own OT transaction (`gCmd`).
- **Fio 5 — album cover art.** QuickTime `GraphicsImporter` decodes the `/cover`
  JPEG (`FF D8` sniff, law 7) into a 64×64 GWorld, drawn in the corner.
- **Fio 6 — preferences.** Server host/port in a `Casquinha Prefs` file
  (`FindFolder`); File▸Preferences (⌘,) dialog; every transaction targets the
  configured `gHost/gPort`.
- **Fio 7 — search + queue.** Non-modal List Manager windows (⌘F / ⌘U) so the
  poll keeps running; `cq_track` parses the `item.<i>.*` lists; double-click a row
  to play (`/spot/play?uri=`, fire-and-forget).
- **Icon.** Classic `ICN#/icl8/ics#/ics8` + `BNDL`/`FREF`/`Casq` signature from
  the otter art (`tools/gen_icons.py`); app built with `CREATOR 'Casq'`.
- **Audio (⌘T) — VM-VERIFIED (b21).** QuickTime is NOT in the audio path; it
  cannot play a length-less live stream on OS 9 (QT 5.0.2). The b13–b21 log
  arc eliminated every QT route: the URL-movie importer parks at
  `kMovieLoadStateLoading` with 0 tracks forever (idle-import flag included),
  and SoundConverter assembles chains for BOTH MP3 fourccs ('.mp3',
  0x6D730055) that consume full-rate input and emit zero PCM, `noErr`,
  forever — pull (FillBuffer + extended record) and push (ConvertBuffer with
  exact cq_mp3_walk frame counts) alike. What works, end to end on our own
  code: `cq_tx_stream` (endless OT read, no watchdog once receiving, 64 KB
  high-water backpressure, incremental drain) → HTTP header skip → `cq_mp3`
  confirmed-frame sync → **minimp3** (public domain, vendored
  `src/minimp3.h`, wrapped in the pure `cq_mp3dec` seam, host-tested against
  a captured slice of the real mount) → a 1.5 MB PCM ring drained by
  **SndPlayDoubleBuffer** (b25). The double-back proc at interrupt ONLY reads
  the ring; the event loop ONLY writes it; a starved ring is served as
  silence with the buffer still marked ready, so the channel never dies and
  playback resumes by itself after menus/drags/dialogs. This replaced a
  bufferCmd + callBackCmd queue whose busy-flag bookkeeping broke under
  pressure (b24 log: "played" outran wall-clock by 33% — flags cleared
  early, queued buffers were overwritten, audio chopped and overlapped).
  Radio physics (b23): a live mount arrives at exactly playback rate, so the
  cushion never grows on its own — PCM is prebuffered before the first note
  and capped at a steady-state target; startup latency IS the freeze
  immunity, and the two knobs (CQ_AUD_PREBUF_PCM / CQ_AUD_RING_TARGET) trade
  one for the other. b28 ran flawlessly at ~6 s (sil 0 across 3.5 min);
  b29 dialed to ~2-3 s by preference, the graceful-starvation behavior
  covering the thinner cushion. The server side (librespot→Icecast) adds a
  ~1-2 s floor no client knob removes. SIZE: 4 MB / 2 MB.
  b31 latency lesson: b29's stop-decoding-at-target "cap" didn't cap anything
  — the backlog parked upstream in staging (4 s) + transport (4 s), invisible
  and untrimmable: perceived control latency hit ~10 s. Now the decoder
  always runs (all backlog lives in the measurable ring) and excess beyond
  target+slack is SKIPPED via a one-shot gRingSkip the interrupt applies —
  a jump-cut back toward the live edge after freeze/dry-spell catch-ups,
  race-free because gRingRd keeps a single writer.
  Mount forensics (tools/mp3scan.c on host captures): the Icecast stream is
  structurally clean even across wake + NO_ACTIVE_DEVICE + skips — one junk
  gap at connect, zero mid-stream gaps, zero format flips, 16.0 KB/s wall.
  The backend's real wart is the wake/transfer STATE dance (wake?play=1 can
  land paused, /next right after can fail NO_ACTIVE_DEVICE) — gopher-spot
  territory, not audio corruption.
- **End-of-track watchdog + visible-queue sequencing (b35/b37/b40).**
  One-call direct play (`play?uri=`) gives Spotify a single-track context:
  librespot STOPS at its end instead of pulling the queue (VM-observed:
  2:20/2:20 frozen, queue full, `/now` said `stopped device active`), the
  server queue is never consumed by jumps, and from a dead context `/next`
  no-ops or fails NO_ACTIVE_DEVICE. So the client sequences the VISIBLE
  queue: `QueueNextRow()` finds the current `track_id` among the rows and
  the watchdog (playing-near-end → stopped, single-shot, re-armed only by
  real playback) plays the NEXT row — "from here onward", exactly what the
  screen shows. The Next button/⌘-key does the same when nothing is playing,
  and falls back to `/next` during live playback. **b41 goes native**: jumps
  send the clicked row + continuation as bare ids to the new
  `/spot/api/1/play/from?ids=…&offset=N` (spec'd from this repo in
  `design/SPEC-play-from.md`, implemented in gopher-spot) — Spotify gets a
  real multi-track context and owns advance/next/prev. Feature-detected: an
  old server answers `not_found` and the client replays the intent as the
  b40 single-uri play (watchdog + sequencer stay as the fallback/safety
  net, dormant when native contexts run).
- **Scriptability + smoke test (b36).** SIZE is now high-level-event aware;
  the app handles the required Apple Events suite (quit et al.) plus
  misc/dosc ("do script") with one command string: listen stop play pause
  next prev wake add search:<q> — each logged as `apple-event: ...`, so a
  scripted run narrates itself in the per-build log. Two AppleScripts ship
  in `tools/` (and on the share as MacRoman/CR .txt for Script Editor):
  "Test Casquinha" drives launch→listen→search→add→next→wake→stop→quit with
  paced delays, and "Collect Logs" has the Finder copy every
  `Casquinha b*.log` onto the AFP share.
- **Player-aware audio status (b49).** "Paused … on air" side by side read
  as stale (the tail really is audible ~3 s — radio latency — but it looks
  wrong). When the player isn't playing, the readout follows the physics:
  "playing out..." while the ring's tail drains, then "standing by" (tuned,
  silent transmitter); "waiting for Spotify..." is reserved for the true
  anomaly (state playing, rx dry).
- **Command ack + radio vocabulary (b48).** Clicking Next showed nothing
  for up to ~7 s (debounce + poll flip + radio latency) — now "Skipping..."
  replaces the state word the instant the click lands, cleared when the
  track_id actually flips (or 8 s timeout). And the audio status went
  lowercase-radio ("on air" instead of "Listening", which read like the
  b46-retired toggle).
- **Auto-start matrix (b47).** The single launch wake hiccupped in the
  field: `wake?play=1` on a PAUSED-but-active device does NOT resume
  (transfer-to-self no-op; the user sat in silence 17 s and pressed Next).
  Now the first snapshot decides like a human: playing→nothing,
  paused-on-device→plain `/play` resume, stopped-on-device→play the visible
  queue head once the first /queue lands (gAutoPlayPending — /queue isn't
  in yet at first-/now time), off-device/idle→wake?play=1.
- **Button retirement (b46).** Auto-start (b43) + the status readout (b45)
  made the Listen/Stop and Wake buttons ceremonial — removed; ⌘T/⌘K and
  the AppleScript commands are the manual levers. The shelf is one row
  (field + Search + Add to Queue) and the lists got the reclaimed 28 px.
- **Audio status readout (b45).** The engine narrates itself in the status
  row (right-aligned, animated by the 2 Hz redraw): Tuning in… →
  Buffering… N% (ring fill vs prebuffer — THE anti-re-click device; a
  second Listen click is a Stop) → Listening, plus "Buffering…" on
  starvation (gDBSilence moved within ~2 s) and "Waiting for Spotify…"
  when rx parks >3 s while playing (dry mount: the right lever is
  Play/Wake, not Listen).
- **Auto-start (b43).** Opening the app is the user intent: auto-listen at
  launch (the engine's graceful dry-mount behavior makes sequencing free —
  it prebuffers until the wake opens the tap) + ONE wake decided off the
  first /now snapshot (skipped when already playing on the device;
  deliberately steals playback from elsewhere — that's the ask). Single-shot
  per launch, never re-fired mid-session — CLIENTS.md rule 10 ("wake only
  after user intent") read honestly: the launch was the intent, once.
  Hold OPTION at launch to start quiet.
- **One-window UI (b30).** Menu tracking is a synchronous loop that starves
  the audio ring, so nothing you'd touch mid-listening lives in a menu
  anymore: search (field + results list), the queue list, Add to Queue, the
  Listen⇄Stop toggle and Wake are all IN the main window (460×490 — player
  on top, shelf below). The separate Search/Queue windows are gone; the File
  menu is just Preferences/Quit; ⌘F/⌘T/⌘K/⌘U survive as hand-rolled
  shortcuts. The 2 Hz diagnostics animator repaints ONLY the player area
  (above CQ_SHELF_TOP) — erasing the shelf would blank the List Manager
  lists twice a second. The queue poll is permanent now (list always
  visible): cadence halved to 10 s per the exhaustion discipline, with the
  post-command kick (Fio H) keeping adds feeling immediate.
  minimp3 wants ~15 KB of stack: `SetApplLimit` carves headroom before
  `MaxApplZone`. Steady state on the VM: ~3.8 bufs/s (= 1.0 s audio/s),
  4 in flight, zero underruns; the mount starves briefly between tracks
  (librespot feeds Icecast only while playing) and the engine rides it out.
  Debug harness: **opt-in via a marker file** (b42) — telemetry runs ONLY
  when "Casquinha Debug" sits next to the app; without it, no log file and
  no datagrams (clean build for any other Mac). With it: `DbgLog` →
  `Casquinha <tag>.log` (one log PER BUILD, b32; flushed per line) AND a
  live UDP mirror to the dev machine (b34/b38 — remote-syslog; `make
  logtail`, which is tools/loglisten.py because macOS `nc -kul` latches
  onto the first sender; T_UDERR must be cleared or one ICMP unreachable
  kills every later send; health counters in the heartbeat). The marker's
  first line may override the mirror target as `host:port`. Share drops
  are versioned: `make app` copies `Casquinha-<tag>.bin/.dsk` alongside
  the unversioned latest.
- **Fio A (exhaustion audit, b8) — cover fail-once + tried-set.** New pure
  module `cq_cache` (fixed-slot FIFO, NULL value = "tried, no image"): every
  /cover outcome is remembered, so a failing fetch is no longer retried every
  loop pass (the retry storm that was exhausting gopher-spot) and each album is
  requested at most once per run (CLIENTS.md checklist 6). Draw looks the cover
  up by the current `album_id`. Host-tested (`cache_test.c`); **needs the UTM
  pass** — see `design/AUDIT-backend-exhaustion.md` for the audit + Fios B–H.
- **Fios B–H (exhaustion audit + CLIENTS.md compliance, b9).** B: the success
  path now completes the orderly release (`OTSndOrderlyDisconnect` on T_ORDREL)
  so the server gets FIN, not RST. C: custom `SIZE` (accept suspend/resume +
  canBackground) + `osEvt` handling — no NEW polls in the background, in-flight
  drains, audio plays on, resume polls immediately. D: seeded positive jitter
  (`cq_backoff_interval_seeded`, host-tested), queue re-fetch backs off on
  errors, chained /next hops paced ~0.5 s. E: automatic starters (cover, queue
  poll) wait above 3 in-flight connections. F: search converts MacRoman→UTF-8
  (second TEC converter) before percent-encoding — `Construção` leaves as
  `constru%C3%A7%C3%A3o`. G: `rate_limited` keeps cadence + snapshot (no
  backoff); command replies push the next /now a full interval out. H: post-add
  queue refetch is a single kick ~2 s later (eventual consistency window).
  All **need the UTM pass**; per-fio verification steps in the audit doc.

## Next (for the human)

- **UTM runtime pass on Fios 4–7 + audio.** Ground truth (§5): one gesture → one
  served command. Watch the audio path especially.
- Possible polish: the otter icon may need a desktop-DB rebuild (⌘⌥ at boot);
  cover disk cache across launches; the gopher browser (a later line).
- Re-sign the autonomous-run commits if desired (they're unsigned; the 1Password
  agent was locked — `git config commit.gpgsign true` and rebase-reword, or leave).
