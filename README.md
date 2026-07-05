# Casquinha

A native **Mac OS 9.2** Spotify remote — *the essential Radinho, on the oldest
machine yet* — speaking the frozen **gopher-spot machine API `/spot/api/1`** over
raw gopher (RFC 1436), LAN-only. It is the PowerPC / Classic sibling of
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

## Status

- **Fio 0 — scaffold.** Repo, dual-family Makefile, fixtures, docs.
- **Fio 1 — pure core + tests.** The Codec / Model / Reconciler layers as plain
  C99 (`src/cq_*.c`), with an offline test suite that runs **on your modern Mac**
  — no Retro68, no emulator (`make test`, 97 checks green). This is the
  offline-forever proof of the pure stack that the pattern promises.

Everything below Fio 1 (the Open Transport wire, the Toolbox now-playing window,
transport controls, cover art, prefs) is the Mac OS 9 rewrite surface, landing
one numbered *fio* at a time. See [`NOTES.md`](NOTES.md) for the roadmap and the
permanent constraints, and [`design/PATTERN-MAP-os9.md`](design/PATTERN-MAP-os9.md)
for the full DeGelato → Mac OS 9.2 mapping.

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
gopher-spot server at `10.0.100.112:70` outbound through the host. That target is
wired up from Fio 3 on (`make app`, once `RETRO68=<toolchain>` is set).

## Network contract (v1, frozen)

- Server `10.0.100.112:70` (LAN only, plain TCP, no TLS). Write `selector\r\n`,
  read to EOF. `/now` returns UTF-8 `key<TAB>value` lines.
- **Ignore unknown keys, tolerate missing ones, key off `state` first** — the API
  is additive; surface growth must never hard-fail the client.
- The server micro-caches `/now` (~1 s); poll at 2 s and never faster.

Capture a fresh fixture from the live server:

```sh
printf '/spot/api/1/now\r\n' | nc 10.0.100.112 70 > tests/Fixtures/now_live.txt
```

## Layout

```
src/
  cq_codec.{h,c}     raw bytes -> {key:value} fields; JPEG magic-byte sniff (pure)
  cq_now.{h,c}       immutable /now snapshot; state-first; interpolation (pure)
  cq_track.{h,c}     item.<i>.* rows for /queue and /search (pure)
  cq_guard.{h,c}     monotonic ts-guard — MANDATORY (law 2) (pure)
  cq_debounce.{h,c}  pre-wire coalescer — cancel != un-send (law 1) (pure)
  cq_pls.{h,c}       first stream URL from a PLS/M3U (pure; for later audio)
tests/
  *_test.c           the offline suite + a tiny runner
  Fixtures/          now_* + queue/search + cover + stream.pls (copied verbatim)
```

Prefix **CQ** / `cq_`. The name is *casquinha* — an ice-cream cone; the humblest
scoop in the freezer, for the oldest Mac in the house.
