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

## Done

- **Fio 3 — the app polls live over Open Transport.** `os9/casquinha.c` is a
  classic-PPC Toolbox app (document window, Apple/File▸Quit menu, `WaitNextEvent`
  loop). The loop drives `cq_transport_ot`: every 2 s (a `TickCount` delta) it
  starts a `/spot/api/1/now` transaction, advances it non-blocking each pass,
  adopts the reply through `cq_guard` (law 2), and renders the snapshot in
  Monaco; `offline - retrying` on failure, keeping the last snapshot. `make app`
  → `Casquinha.bin` (MacBinary PPC, ~85 KB) + `.dsk`, builds warning-free and is
  on the share. `cq_transport_ot.c` is **compile + link verified** against Apple's
  real OT headers/libs; runtime is the UTM victory lap.

## Next

- **UTM runtime pass:** launch `Casquinha` in the OS 9 VM, confirm live
  now-playing from `10.0.100.112:70`, and check §5 ground truth (one gesture →
  one served command) once controls land.
- Render polish: TEC UTF-8→MacRoman at the draw boundary (accents), and the
  1 Hz interpolation tick between polls.
- Fio 4 (transport controls: prev/play-pause/next/seek/volume + `cq_debounce`),
  5 (cover via QuickTime), 6 (prefs + cache), 7 (search + queue). Then the classic
  `ICN#`/`icl8` app icon (Rez the otter in), the gopher browser, and audio.
