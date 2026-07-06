# SPEC: `/spot/api/1/play/from?ids=…&offset=N` — native "play from here onward"

*A handoff spec for gopher-spot. Written from the Casquinha (Mac OS 9 client)
side, where the absence of this endpoint has been reproduced and worked
around; file references below are into the gopher-spot repo as of
2026-07-06.*

## Why (the field evidence)

The v1 machine API can resume (`play`), skip (`next`/`prev`), and enqueue
(`queue/add`) — but it **cannot start a specific track with a
continuation**. Clients jump to a track via the single-uri path, Spotify gets
a **single-track context**, and then:

- librespot **stops at the track's end** instead of pulling the queue
  (VM-observed: progress frozen at 2:20/2:20, `/now` = `state stopped,
  device active`, queue full);
- `/next` from that dead context **no-ops or fails** `NO_ACTIVE_DEVICE`;
- the server-side queue is never consumed by jumps.

Casquinha b35–b40 papers over this with an end-of-track watchdog and a
client-side "visible queue sequencer" — exactly the chained-command
sequencing that gopher-spot's own API.md rejects on principle ("we do not
emulate it with chained skips"). The native fix is one additive endpoint.

## The endpoint

```
/spot/api/1/play/from?ids=<id1>,<id2>,…,<idK>&offset=<n>
```

- `ids` — comma-joined **bare base62 track IDs** (the 22-char tail of
  `spotify:track:<id>`), 1 ≤ K ≤ **24**. Bare IDs, not full URIs: geomyidae
  v0.99's request-line buffer is the real cap on selector length, and 24
  bare IDs ≈ 580 bytes stays comfortably inside it.
- `offset` — 0-based index into the list; optional, default `0`;
  `error bad_range` when ≥ K.
- The existing single-`uri` form of the human `/spot/play` and everything
  else in v1: **untouched**. This is additive (v1-legal per API.md
  Versioning; precedent: new endpoints stay in v1, clients ignore unknowns).
- **Why not overload `/spot/api/1/play`?** `DcgiArgs::path()` strips the
  query, so on an OLD server `play?ids=…` silently matches the existing
  resume arm — clients could never feature-detect. A new sub (`play/from`,
  mirroring the `queue/add` naming) makes old servers answer `not_found`,
  which is the client's clean fallback signal.

### Semantics

One Spotify Web API call:

```
PUT /v1/me/player/play?device_id=<cached gopher-spot device id>
{"uris": ["spotify:track:<id1>", …, "spotify:track:<idK>"],
 "offset": {"position": N}}
```

Spotify then owns the continuation: auto-advance at each track end,
`next`/`prev` move within the list, no client sequencing anywhere.

### Reply

The command convention: a **fresh `/now` snapshot** (via `fresh_now()`,
src/api.rs:397) so the client adopts the result through its ts-guard without
a follow-up poll. Also **bust the queue cache**, as `next`/`prev` already do
(src/api.rs:328).

### Errors

- `bad_query` — missing/empty `ids`.
- `bad_uri` — any id that is not plausibly base62 (`[0-9A-Za-z]{22}`).
- `bad_range` — `offset` ≥ K (or non-numeric).
- Standard upstream mapping otherwise: `rate_limited` (429 breaker),
  `no_device`, `upstream` — all existing machinery via `send_json` /
  `upstream()` (src/api.rs:489).

### Wire examples

```
/spot/api/1/play/from?ids=7hQJA50XrCWABAu5v6QZ4i,22HMAUrbbYSj9NiPPlGumy&offset=0
→ api	1
  state	playing
  track	Don't Stop Me Now - Remastered 2011
  …full /now snapshot…
  ts	1783365000000

/spot/api/1/play/from?ids=7hQJA50XrCWABAu5v6QZ4i&offset=3
→ api	1
  error	bad_range
  message	offset 3 is outside the 1-item list
```

## Implementation pointers (from recon of this repo)

1. **Builder** — add `SpotifyApi::play_uris(ids: &[String], offset: u32)`;
   the body-builder mirrors `Client::play_context` (src/spotify.rs:846)
   swapping `context_uri` for the `uris` array; send via `send_json`
   (src/spotify.rs:597 — 429 cooldown breaker included for free); device id
   via the cached `device_id()` like `play`/`play_context`.
2. **Route arm** — `api::route` match (src/api.rs:55-72). Parse `ids` with
   `DcgiArgs::query("ids")` (src/dcgi.rs:70 already splits `&` and
   URL-decodes) + a comma split; validate each id (reuse the shape checks
   near `is_track_uri`/`id_from_uri`, src/api.rs:258).
3. **Fakes** — both `#[cfg(test)]` fakes implement `SpotifyApi`
   (src/api.rs:522+, src/dcgi.rs:873+) and need the new method (they already
   stub `play_context`).
4. **Tests** — route: happy path returns a /now doc; `bad_query`/`bad_uri`/
   `bad_range`; builder: body JSON shape (`uris` array + `offset.position`).
5. **Docs** — API.md: add under v1 with the additive note; CLIENTS.md: add
   guidance — *"jumping into a list: send the tail (or the list + offset) as
   `ids=`; do NOT chain `/next` and do NOT sequence client-side."*

## Client adoption (for reference)

Casquinha b41 feature-detects: it sends `play/from?ids=…` and, on
`error not_found` (an older server), falls back to its b40 behavior
(single-uri + client sequencer). No deploy coordination needed in either
direction.
