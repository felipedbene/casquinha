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
- **Audio (⌘T).** QuickTime URL movie from `/spot/stream.pls` (`cq_pls`). **The
  one unverified piece** — classic-QT Icecast streaming is finicky; needs VM
  testing, may need a Sound Manager / movie-import fallback.

## Next (for the human)

- **UTM runtime pass on Fios 4–7 + audio.** Ground truth (§5): one gesture → one
  served command. Watch the audio path especially.
- Possible polish: the otter icon may need a desktop-DB rebuild (⌘⌥ at boot);
  cover disk cache across launches; the gopher browser (a later line).
- Re-sign the autonomous-run commits if desired (they're unsigned; the 1Password
  agent was locked — `git config commit.gpgsign true` and rebase-reword, or leave).
