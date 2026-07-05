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

**Interfaces caveat (load-bearing):** Retro68 ships the open-source **Multiversal
Interfaces**, which have the full Toolbox (Window/Menu/Dialog/QuickDraw/Events/
Fonts…) but **NOT Open Transport, MacTCP, or Carbon**. So the Toolbox UI builds
today; the live network wire does not. Getting OT needs Apple's Universal
Interfaces (from the MPW Golden Master) folded in with `--universal`, or a
hand-rolled minimal OT header + CFM import stub. Deferred — see Fio 3.

## Done

- **Fio 3 (first slice) — the app runs.** `os9/casquinha.c` is a classic-PPC
  Toolbox app: a document window, an Apple/File▸Quit menu bar, a `WaitNextEvent`
  loop, rendering a now-playing snapshot **through the real pure core**
  (`cq_codec`→`cq_now`) in Monaco. `make app` → `Casquinha.bin` (MacBinary PPC
  app) + `.dsk`, verified to build clean and dropped on the share for UTM.
  Snapshot is a baked `/now` fixture for now (no OT yet — see caveat above).

## In progress / Next

- **Fio 2 tail — the OT wire.** `cq_transport_posix.c` is verified on the host
  (`make test` 106 checks + `make probe` against the live server).
  `cq_transport_ot.c` is written but **cannot compile without OT headers**
  (Multiversal gap above). Next: fold in Apple's Universal Interfaces (or a
  minimal OT import stub), then swap the baked fixture in `casquinha.c` for a
  2 s poll → `cq_guard` → render, with TEC UTF-8→MacRoman at the draw boundary.
- Then Fio 4 (transport controls), 5 (cover), 6 (prefs + cache), 7 (search +
  queue). Gopher browser and audio come later.
