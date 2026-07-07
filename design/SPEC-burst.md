# SPEC: Icecast `burst-size` / `queue-size` for the freeze-immunity cushion

*A handoff spec for gopher-spot, written from the Casquinha (Mac OS 9 client)
side. Config-only — no code. Evidence below is from the exp/mp-decode VM
sessions of 2026-07-06 (log-sink, b52).*

## Why (the field evidence)

Casquinha b52 moved MP3 decode to a preemptive MP task, so the client can now
keep playing through the cooperative loop's freezes (menu tracking, window
drags) — **if it has compressed bytes on hand**. It doesn't: the client's
entire cushion is what Icecast bursts at connect, and today that is small.

Measured on the b52 session (94130s):

- The connect burst decoded to ~390 KB of PCM ≈ **2.3 s** of audio (≈ 35-40 KB
  compressed). A live mount then arrives at exactly playback rate, so the
  cushion never grows past that.
- A 17.5 s menu freeze: 2.3 s of music, then ~15 s of silence (`sil 0→80`) —
  the decode task drained its ~750 B of staged input in the first second and
  ran dry while ~280 KB sat unreachable in the frozen socket.
- A ~36 s freeze: **Icecast dropped the connection** (`stream closed by
  server`) — the slow-client queue limit. Long freezes don't just go silent,
  they disconnect.

The client side is done: b53 holds a deep standing backlog (2 MB PCM ring,
trim target ~7.4 s) and jump-cuts to the live edge only when the user issues
a transport command. What's missing is the fuel at connect.

## The ask (icecast.xml on the gopher-spot deployment)

```xml
<burst-size>262144</burst-size>          <!-- ~256 KB ≈ 16 s at 128 kbps -->
<queue-size>2097152</queue-size>         <!-- ~2 MB ≈ 2 min of client lag
                                              before disconnect -->
```

- `burst-size` ~256 KB fills the b53 client cushion (~7.4 s of PCM + comp-ring
  headroom) in the first second of a connection. Larger is harmless — the
  client back-pressures via TCP once its rings are full.
- `queue-size` ≥ 2 MB keeps Icecast from killing a client frozen for tens of
  seconds (the 36 s disconnect above). The frozen client's TCP window closes;
  the queue absorbs the difference.

## Cost / risk

- Memory: per listener, worst case ~2 MB server-side queue. Casquinha is the
  only listener class on this mount.
- Latency: none imposed — burst is replay of already-sent audio; clients that
  want the live edge simply skip it (Casquinha trims client-side).
- No API change, no restart beyond the Icecast config reload.

## Acceptance (client-observable)

After the change, a fresh Casquinha listen should log a connect burst that
fills the ring well past the 384 KB prebuffer (watch `audio: PLAY, ring N KB
deep` → expect N near the 1280 KB target, not ~320), and a 10+ s menu hold
while playing should show `sil` NOT incrementing (the b52 task decodes
through the freeze on burst fuel). A ~1 min freeze should survive without
`stream closed by server`.
