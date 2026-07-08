# OTConnect freeze — captured evidence (b59 audit → b60 probe → b61 fix)

Source: k8s log-sink (gopher-spot/log-sink), VM mirror 10.0.10.5. Archived 2026-07-08.
Raw stream: freeze-sink-2026-07-08.log(.gz). Probe threshold: >100ms per OT trap.

## 1. b60 — the freeze caught red-handed (rollout)
Loop heartbeat (\`audio: playing\`) goes dark ~110s; MP task (\`mp\`) keeps beating; rx flushes on recovery.
```
10.0.10.5 [3552.01s] audio: playing, ring 722 KB, fires 8058, sil 811, trims 77, dec 57168f/133066ms, pcm 257256K, 2173 staged, 23898244 rx, mp 228978, udp 249/0 e0
10.0.10.5 [3400.96s] audio: playing, ring 596 KB, fires 803, sil 0, trims 6, dec 6353f/14547ms, pcm 28588K, 781 staged, 2657444 rx, mp 22126, udp 37/0 e0
10.0.10.5 [3410.96s] audio: playing, ring 587 KB, fires 857, sil 0, trims 6, dec 6735f/15522ms, pcm 30307K, 720 staged, 2817044 rx, mp 23608, udp 38/0 e0
10.0.10.5 [3420.96s] audio: playing, ring 578 KB, fires 911, sil 0, trims 6, dec 7117f/16443ms, pcm 32026K, 2060 staged, 2978044 rx, mp 25094, udp 40/0 e0
10.0.10.5 [3430.96s] audio: playing, ring 601 KB, fires 964, sil 0, trims 6, dec 7499f/17392ms, pcm 33745K, 600 staged, 3136244 rx, mp 26583, udp 41/0 e0
10.0.10.5 [3440.96s] audio: playing, ring 592 KB, fires 1018, sil 0, trims 6, dec 7881f/18364ms, pcm 35464K, 539 staged, 3295844 rx, mp 28076, udp 42/0 e0
10.0.10.5 [3550.78s] ot-probe: OTConnect +6313t (105216ms)
10.0.10.5 [3550.80s] audio: playing, ring 12 KB, fires 1608, sil 547, trims 6, dec 8058f/18820ms, pcm 36261K, 66296 staged, 3435580 rx, mp 46996, udp 44/0 e0
```
The single un-deadlined synchronous OTConnect issue blocked the cooperative loop 105.2s.

## 2. b61 — same rollout, no wedge
```
10.0.10.5 [3807.85s] audio: playing, ring 492 KB, fires 589, sil 0, trims 7, dec 4829f/11246ms, pcm 21730K, 662 staged, 2020444 rx, mp 16078, udp 35/0 e0
10.0.10.5 [3817.85s] audio: playing, ring 483 KB, fires 643, sil 0, trims 7, dec 5211f/12263ms, pcm 23449K, 602 staged, 2180044 rx, mp 17557, udp 36/0 e0
10.0.10.5 [3827.85s] audio: playing, ring 506 KB, fires 696, sil 0, trims 7, dec 5593f/13233ms, pcm 25168K, 542 staged, 2339644 rx, mp 19033, udp 37/0 e0
10.0.10.5 [3837.85s] audio: playing, ring 497 KB, fires 750, sil 0, trims 7, dec 5975f/14190ms, pcm 26887K, 481 staged, 2499244 rx, mp 20516, udp 38/0 e0
10.0.10.5 [3847.85s] audio: playing, ring 488 KB, fires 804, sil 0, trims 7, dec 6357f/15168ms, pcm 28606K, 421 staged, 2658844 rx, mp 21994, udp 39/0 e0
10.0.10.5 [3853.38s] now: 10:35 - Ti√´sto, Tate McRae
10.0.10.5 [3857.85s] audio: playing, ring 506 KB, fires 857, sil 0, trims 7, dec 6738f/16138ms, pcm 30321K, 778 staged, 2818444 rx, mp 23484, udp 41/0 e0
```

## 3. b61 — 90s FULL backend blackout (endpoints=<none>), no wedge
Clean 10s heartbeat cadence, sil 0, zero probe, through a total control-plane outage.
```
10.0.10.5 [4157.51s] audio: playing, ring 651 KB, fires 21059, sil 805, trims 19, dec 145303f/705828ms, pcm 653863K, 2019 staged, 60735124 rx, mp 417275, udp 459/0 e0
10.0.10.5 [4218.03s] audio: playing, ring 650 KB, fires 21384, sil 805, trims 19, dec 147614f/718020ms, pcm 664263K, 715 staged, 61699724 rx, mp 423439, udp 465/0 e0
10.0.10.5 [4288.03s] audio: playing, ring 647 KB, fires 21760, sil 805, trims 19, dec 150287f/732279ms, pcm 676291K, 711 staged, 62816924 rx, mp 430579, udp 472/0 e0
10.0.10.5 [4297.90s] audio: playing, ring 513 KB, fires 3220, sil 0, trims 7, dec 23543f/61824ms, pcm 105943K, 774 staged, 9842244 rx, mp 87724, udp 87/0 e0
```

## 4. Every ot-probe hit in the capture
```
10.0.10.5 [661.96s] ot-probe: OTConnect +7t (116ms)
10.0.10.5 [663.71s] ot-probe: OTConnect +13t (216ms)
10.0.10.5 [665.81s] ot-probe: OTConnect +13t (216ms)
10.0.10.5 [728.30s] ot-probe: OTConnect +52t (866ms)
10.0.10.5 [1128.33s] ot-probe: OTConnect +10t (166ms)
10.0.10.5 [1131.46s] ot-probe: OTConnect +71t (1183ms)
10.0.10.5 [1158.83s] ot-probe: OTConnect +8t (133ms)
10.0.10.5 [1159.66s] ot-probe: OTConnect +11t (183ms)
10.0.10.5 [1164.40s] ot-probe: OTConnect +9t (150ms)
10.0.10.5 [1166.51s] ot-probe: OTConnect +8t (133ms)
10.0.10.5 [1471.53s] ot-probe: OTConnect +9t (150ms)
10.0.10.5 [1482.36s] ot-probe: OTConnect +6t (100ms)
10.0.10.5 [1501.30s] ot-probe: OTConnect +13t (216ms)
10.0.10.5 [1547.03s] ot-probe: OTConnect +10t (166ms)
10.0.10.5 [1967.20s] ot-probe: OTConnect +15t (250ms)
10.0.10.5 [1967.70s] ot-probe: OTConnect +11t (183ms)
10.0.10.5 [1978.00s] ot-probe: OTConnect +9t (150ms)
10.0.10.5 [1978.53s] ot-probe: OTConnect +11t (183ms)
10.0.10.5 [1980.11s] ot-probe: OTConnect +9t (150ms)
10.0.10.5 [1984.33s] ot-probe: OTConnect +10t (166ms)
10.0.10.5 [1986.43s] ot-probe: OTConnect +12t (200ms)
10.0.10.5 [1995.55s] ot-probe: OTConnect +10t (166ms)
10.0.10.5 [1997.65s] ot-probe: OTConnect +10t (166ms)
10.0.10.5 [1999.56s] ot-probe: OTConnect +13t (216ms)
10.0.10.5 [2006.91s] ot-probe: OTConnect +59t (983ms)
10.0.10.5 [2010.06s] ot-probe: OTConnect +16t (266ms)
10.0.10.5 [3550.78s] ot-probe: OTConnect +6313t (105216ms)
```
