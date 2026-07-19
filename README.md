# Casquinha

A native **Mac OS 9.2** Spotify remote — *the essential Radinho, on the oldest
machine yet* — speaking the frozen
**[gopher-spot](https://github.com/felipedbene/gopher-spot) machine API
`/spot/api/1`** over raw gopher (RFC 1436), LAN-only. It is the PowerPC / Classic
sibling of
[DeToca](https://github.com/felipedbene/detoca) (Snow Leopard 10.6 / i386) and
[DeGelato](https://github.com/felipedbene/degelato) (Sorbet Leopard 10.5 / ppc),
and the third reference implementation of the umbrella recipe,
[**fhb ▸ CLIENT-PATTERN.md**](https://github.com/felipedbene/fhb/blob/main/CLIENT-PATTERN.md).

The pattern's whole claim is that the hard part — staying *coherent* against a
replicated, eventually-consistent backend over a protocol that cannot un-send a
command — lives in a handful of **pure** layers and seven reconciliation laws
that come from the backend and the protocol, **not the OS**. So they port
verbatim; only the glue and the platform escape-hatches change. Casquinha is that
claim taken to its limit: no Cocoa, no Objective-C, no BSD sockets, no threads —
just plain C, **Open Transport**, and the **Toolbox** on a cooperative event loop.

## What it does

A full native remote for the gopher-spot bridge, driving Spotify and showing
state entirely in the Toolbox:

- **Now Playing** — a Platinum window polling `/now` every 2 s over Open
  Transport, interpolating the progress bar from `ts` between polls, with a
  monotonic guard so a stale reply never rewinds the display. TEC UTF-8→MacRoman
  for accented names; graceful 429 backoff.
- **Transport** — prev / play-pause / next, a volume slider, and a click-to-seek
  progress bar.
- **Opens ready to play** — launching the app IS the intent: it tunes the
  stream immediately and, off the first `/now` snapshot, fires ONE wake if
  playback isn't already on the gopher-spot device. Zero clicks to música.
  Hold **⌥ Option** at launch to start quiet.
- **Wake** (⌘K) — transfer playback onto the gopher-spot librespot device so
  the audio stream carries it, recovering the "playing on another device"
  idle state (manual re-wake mid-session; the launch wake is automatic, and
  the buttons retired in b46 — the app runs itself).
- **Search & Queue** — built into the main window (search field + results list,
  live queue list); double-click a search hit to play it now and continue with
  the queue, or a queue row to play **from there onward** — both as native
  Spotify multi-track contexts via `/spot/api/1/play/from`, so advance, next
  and prev behave natively. Nothing playback-related lives in a menu: on a
  cooperative OS, menu tracking freezes the app — and the audio with it.
- **Cover art** — QuickTime GraphicsImporter, behind a fail-once cover cache.
- **Preferences** (⌘,) — File▸Preferences opens a hand-editable config file
  (server `host:port`) in SimpleText; edit it, save, and switch back to
  Casquinha to apply on resume (no modal dialog). Saved to disk.
- **Audio** (automatic; ⌘T toggles manually) — with a live, player-aware
  status readout in radio vocabulary: `tuning in… → buffering… N% → on air`,
  `playing out… / standing by` when upstream pauses, `waiting for Spotify…`
  for the genuine anomaly (a **server fact** from `/spot/api/1/stream` when
  the server has it; a receive-side heuristic on older servers) — and an
  instant `Skipping...` acknowledgment when you hit Next/Prev. Everything
  the player area says is decided by ONE pure, host-tested module
  (`cq_view`), so the state word, status line and ack can't contradict each
  other on screen. The stream is the live Icecast MP3, decoded **in-app by
  minimp3** and played through the Sound Manager (`SndPlayDoubleBuffer` fed
  from a PCM ring, ~3 s radio latency with graceful starvation). QuickTime
  proved unable to open a length-less live stream on OS 9, so the whole path
  — endless Open Transport read, MP3 frame sync, decode, output — is the
  project's own code (see NOTES.md for the b13–b49 arc).

Under all of it: **backend-exhaustion hardening + CLIENTS.md compliance** —
orderly TCP release, suspend/resume, jittered backoff, an in-flight cap, UTF-8
search encoding, `rate_limited` degradation, and a single polite queue re-poll
after a change. See [`design/AUDIT-backend-exhaustion.md`](design/AUDIT-backend-exhaustion.md).

## Status

The pure core (Codec / Model / Reconciler) is plain C99 with an offline suite
that runs **on a modern Mac** — no Retro68, no emulator (`make test`, **311
checks green**, including decoding a captured slice of the real stream). The
app is classic PowerPC + Open Transport + the Toolbox, cross-built with
Retro68 and run in **UTM** (QEMU/PPC).

Exercised on the VM (through b63; binaries on the
[releases page](https://github.com/felipedbene/casquinha/releases)):
auto-start, Now Playing, transport, search, queue (add + jump with **native
play-from contexts** via gopher-spot's `/spot/api/1/play/from`, spec'd from
this repo in [`design/SPEC-play-from.md`](design/SPEC-play-from.md)), covers,
preferences, wake, and the full audio path (sustained playback, zero
underruns). **Open item:** the full **Fios A–H** runtime pass on real
hardware.

See [`NOTES.md`](NOTES.md) for the fio-by-fio arc + permanent constraints, and
[`design/PATTERN-MAP-os9.md`](design/PATTERN-MAP-os9.md) for the DeGelato →
Mac OS 9.2 mapping.

## Debugging & telemetry (opt-in)

The app is silent by default — **no log files, no network telemetry**. To
turn the debug harness on, put a file named **`Casquinha Debug`** in the same
folder as the app (an empty SimpleText file is enough). With the marker
present:

- every event is appended to **`Casquinha <tag>.log`** next to the app (one
  log per build, flushed per line so it survives a freeze), and
- each line is **mirrored live as a UDP datagram**. Two sinks exist: the
  cluster's always-on **log-sink** (a MetalLB service at
  `<log-sink-host>:5514`, deployed with gopher-spot; read it with
  `kubectl -n gopher-spot logs -f deploy/log-sink` — lines are prefixed
  with the sender IP so all family clients can share it), or an ad-hoc
  `make logtail` on the dev Mac (a tiny Python listener; macOS `nc -kul`
  latches onto the first sender and drops the rest). The marker file's
  first line selects the target as `host:port` — put
  `<log-sink-host>:5514` in it for the cluster sink.

Delete the marker and the app goes quiet again. Extras in `tools/`:
`mp3scan.c` (mount forensics: frame gaps/format flips in a captured stream),
and two AppleScripts — `Test Casquinha` (an ordered smoke test driven over
Apple Events: the app answers `quit` and a `do script` command string) and
`Collect Logs` (Finder copies every per-build log onto the AFP share).
`make app` drops **versioned** binaries (`Casquinha-<tag>.bin/.dsk`) on the
share alongside the unversioned latest, and the build tag is visible in the
status row — no more guessing which binary is running over there.

## Building

```sh
make test        # build + run the pure-core suite on THIS Mac (offline, no Retro68)
make clean
```

The pure core is deliberately host-buildable: the seam between Transport and
Codec is where the tests live (CLIENT-PATTERN.md §1), so the Codec/Model/
Reconciler layers compile and run against the copied fixtures with the system
`cc`, forever, with no classic hardware in the loop.

The Mac OS 9 app itself cross-builds with the **Retro68** GCC toolchain
(PowerPC/CFM + the Rez resource compiler + Universal Interfaces) and runs in
**UTM** (QEMU/PPC) with shared networking, so the OS 9 guest reaches the
gopher-spot server at `<spot-host>:70` outbound through the host. That target is
wired up from Fio 3 on (`make app`, once `RETRO68=<toolchain>` is set).

## Network contract (v1, frozen)

- Server `<spot-host>:70` (LAN only, plain TCP, no TLS). Write `selector\r\n`,
  read to EOF. `/now` returns UTF-8 `key<TAB>value` lines.
- **Ignore unknown keys, tolerate missing ones, key off `state` first** — the API
  is additive; surface growth must never hard-fail the client.
- The server micro-caches `/now` (~3 s); poll at 2 s and never faster.

Capture a fresh fixture from the live server:

```sh
printf '/spot/api/1/now\r\n' | nc <spot-host> 70 > tests/Fixtures/now_live.txt
```

## Layout

```
src/
  cq_codec.{h,c}         raw bytes -> {key:value} fields; JPEG magic-byte sniff (pure)
  cq_now.{h,c}           immutable /now snapshot; state-first; interpolation (pure)
  cq_track.{h,c}         item.<i>.* rows for /queue and /search (pure)
  cq_guard.{h,c}         monotonic ts-guard — MANDATORY (law 2) (pure)
  cq_debounce.{h,c}      pre-wire coalescer — cancel != un-send (law 1) (pure)
  cq_backoff.{h,c}       exponential poll backoff + seeded jitter (pure)
  cq_cache.{h,c}         fixed-slot FIFO cover cache; fail-once semantics (pure)
  cq_pls.{h,c}           first stream URL from a PLS/M3U (pure; audio)
  cq_mp3.{h,c}           MP3 frame-header parse / confirmed-frame sync (pure; audio)
  cq_mp3dec.{h,c}        the decode seam wrapping the vendored minimp3 (pure; audio)
  minimp3.h              vendored public-domain MP3 decoder (one TU, cq_mp3dec.c)
  cq_transport.h         the transport seam (+ streaming mode, + UDP log mirror)
  cq_transport_ot.c      Open Transport state machine (OS 9, sync+non-blocking)
  cq_transport_posix.c   BSD-socket twin so the seam is host-testable
os9/
  casquinha.c            the app: Toolbox glue, event loop, audio engine (Retro68)
  casquinha.r, icon.r    resources + the otter icon family
tests/
  *_test.c               the offline suite + a tiny runner
  Fixtures/              now_* + queue/search + cover + stream.pls + stream.mp3
                         (a captured slice of the real mount, decoded in the suite)
tools/
  mp3scan.c              mount forensics: frame gaps / format flips in a capture
  loglisten.py           UDP log listener (make logtail)
  *.applescript          smoke test over Apple Events + Finder log collection
design/
  AUDIT-backend-exhaustion.md   exhaustion audit + Fios A–H (UTM pass pending)
  PATTERN-MAP-os9.md            DeGelato -> Mac OS 9.2 mapping
  SPEC-play-from.md             the cross-repo spec that became /spot/api/1/play/from
  vm-logs/                      the b13–b38 field logs behind the audio arc
```

Prefix **CQ** / `cq_`. The name is *casquinha* — an ice-cream cone; the humblest
scoop in the freezer, for the oldest Mac in the house.
