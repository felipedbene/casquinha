# Cushion physics — post-mortem of the b53/b54 live-cut (exp/mp-decode)

*Written 2026-07-06 after two field failures on the same VM day. This is the
note to reread before ever again proposing "deep freeze immunity AND fast
commands".*

## The conservation law

For a live Icecast mount, audio arrives at exactly playback rate. Therefore:

```
ear-lag behind the live edge  ==  client-side cushion (freeze immunity)
```

- The cushion **never grows on its own** — it is set once, by the connect
  burst, and every discard (trim/jump-cut) spends it **for the rest of the
  connection**.
- Deepening it seamlessly is impossible: you'd have to play slower than
  arrival (pitch) or replay already-heard audio (the reconnect trap).
- A deep cushion also desyncs UI from ear: `/now` flips ~cushion seconds
  before the listener hears the transition (the b44 staleness, amplified).

So "menu holds survive 8 s" and "Next reaches the ear in 3 s" are the same
knob pointed in opposite directions. Any scheme that promises both is really
choosing **where the discontinuity lands** — and the field showed every
client-side choice of discontinuity misfires:

## What was tried and how it failed (b53/b54, VM logs 2026-07-06)

1. **Cut-on-send (b53)**: transport command → immediate jump-cut to ~2 s.
   Failure: `/next` can silently no-op upstream (the b37 dead-context
   disease — this session's context came from wake-on-launch). The listener
   got an audible 6 s mid-song jump, no track change behind it, and the
   connection's cushion was spent for nothing. (Log: `live-cut, skipping
   1022 KB` at 95589.96, then 70 s with no `now:` flip.)

2. **Cut-on-confirmation (b54)**: command only arms; the cut fires when the
   adopted snapshot shows a track/state flip. Failure: **attribution**. With
   a deep cushion, *natural* track changes flip `/now` ~7 s before the ear
   reaches them; one raced a pending Next during a menu freeze and the cut
   stole exactly the transition the listener was about to hear (log
   96155-96156: natural flip → `live-cut, skipping 1215 KB`). A third arm
   expired 3 s before its real flip arrived. Timing races everywhere.

3. Also observed: after the one legitimate cut, the cushion sat at ~2 s for
   the rest of the connection (law above), so the next menu hold starved
   after 2 s — the immunity the whole experiment was for.

## Where b55 landed

Shallow, proven, honest: `CQ_AUD_RING_TARGET` back to 512 KB (~2.9 s), no
cuts except the standing near-full guard. What the experiment keeps:

- **b52 preemptive decode** — decode never stops during freezes; recovery is
  instant; short holds (< cushion) are fully seamless.
- **SPEC-burst queue-size** — Icecast no longer drops a client frozen for
  tens of seconds (the 36 s `stream closed by server`).
- The 2 MB ring and the `cq_decring` module stay: capacity for any future
  profile without re-surgery.

## The remaining honest options (pick one knowingly, or leave it)

- **A. Shallow (b55, current)**: Next ≈ 4 s to the ear, immunity ≈ 3 s.
  The historical feel; b48's instant status ack covers the wait.
- **B. Deep, no cuts**: retarget to ~1.2 MB. Immunity ≈ 7 s, every command
  ≈ 9 s to the ear, and the UI leads the ear by ~7 s everywhere (a b49-class
  honesty problem). One-knob change if ever wanted.
- **C. Re-tune-on-command**: on a *confirmed* command, drop the stream and
  reconnect (fresh burst = cushion regrows, always lands at the live edge).
  Still needs attribution (the b54 problem), still replays or gaps ~2 s, and
  a post-connect trim decides lag all over again. Not obviously better than
  A; prototype only with a concrete UX target.
- **D. Rebuild-at-silence**: deepen the cushion by stretching inter-track
  gaps or "standing by" silence (replaying silence is inaudible). Correct in
  principle, needs silence detection in the pipeline. Future material.
