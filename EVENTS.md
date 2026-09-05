# Event ledger (authoritative, regenerated from console logs)

Regenerated 2026-09-05 by decomposing raw counter captures. Supersedes
all hand-tallied counts in FINDINGS. Testbed uses per-event esp_timer
deltas (deficits stack -- no clock_guard); garage uses per-repair PRE-REPAIR
raws. Bits below 26 excluded (never cleared; low diffs are capture skew).

## Testbed -- 24 bit-verified events

| # | magnitude | cleared bits | span | crosses 35/36 |
|--|--|--|--|--|
| TB-01 | 0.14 min | [27] | (27, 27) | no |
| TB-02 | 0.28 min | [28] | (28, 28) | no |
| TB-03 | 0.98 min | [29, 28, 27] | (27, 29) | no |
| TB-04 | 3.64 min | [31, 30, 28] | (28, 31) | no |
| TB-05 | 6.01 min | [32, 30, 28, 27] | (27, 32) | no |
| TB-06 | 6.50 min | [32, 30, 29, 28, 26] | (26, 32) | no |
| TB-07 | 12.65 min | [33, 31, 30, 28, 26] | (26, 33) | no |
| TB-08 | 20.20 min | [34, 31, 26] | (26, 34) | no |
| TB-09 | 21.67 min | [34, 31, 30, 28, 27] | (27, 34) | no |
| TB-10 | 25.24 min | [34, 32, 31, 29, 26] | (26, 34) | no |
| TB-11 | 27.05 min | [34, 33, 27, 26] | (26, 34) | no |
| TB-12 | 33.20 min | [34, 33, 32, 30, 29, 27, 26] | (26, 34) | no |
| TB-13 | 53.13 min | [35, 33, 32, 31, 30, 29] | (29, 35) | no |
| TB-14 | 53.76 min | [35, 34, 26] | (26, 35) | no |
| TB-15 | 53.76 min | [35, 34, 26] | (26, 35) | no |
| TB-16 | 54.60 min | [35, 34, 29, 28, 26] | (26, 35) | no |
| TB-17 | 71.58 min | [36] | (36, 36) | no |
| TB-18 | 143.17 min | [37] | (37, 37) | no |
| TB-19 | 429.50 min | [38, 37] | (37, 38) | no |
| TB-20 | 11.9 h | [39, 37] | (37, 39) | no |
| TB-21 | 21.5 h | [40, 37] | (37, 40) | no |
| TB-22 | 492.3 d | [49, 46, 45, 43, 41, 40] | (40, 49) | no |
| TB-23 | 1627.3 d | [50, 49, 48, 47, 46, 45, 44, 43, 42, 41] | (41, 50) | no |
| TB-24 | 1628.1 d | [50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40] | (40, 50) | no |

## Garage -- 11 bit-verified (of 17 corrections; 6 raws lost to log gaps) + 4 non-captured faults

| # | magnitude | cleared bits | span | crosses 35/36 |
|--|--|--|--|--|
| GA-01 | 8.95 min | [33] | (33, 33) | no |
| GA-02 | 8.95 min | [33] | (33, 33) | no |
| GA-03 | 9.72 min | [33, 29, 27, 26] | (26, 33) | no |
| GA-04 | 15.31 min | [33, 32, 30, 29, 27, 26] | (26, 33) | no |
| GA-05 | 19.57 min | [34, 30, 29] | (29, 34) | no |
| GA-06 | 29.36 min | [34, 33, 31, 28] | (28, 34) | no |
| GA-07 | 32.65 min | [34, 33, 32, 30, 27, 26] | (26, 34) | no |
| GA-08 | 40.06 min | [35, 31, 30, 29, 28, 26] | (26, 35) | no |
| GA-09 | 40.27 min | [35, 32] | (32, 35) | no |
| GA-10 | 56.27 min | [35, 34, 31, 28, 26] | (26, 35) | no |
| GA-11 | 31.0 h | [40, 39, 37] | (37, 40) | no |

## Totals

- **Bit-level-verified: 35** (24 testbed + 11 garage)
- Garage magnitude-only (raw lost): 6; non-correction (1 refused 62h + 3 pre-clock_guard): 4
- **Grand total faults across both boards: ~45 (45)**
- **35/36 crossings: 0** (2 apparent crossings during the 09-05 audit were artifacts: one my cumulative-deficit decomposition bug, one a leaked synthetic alert-test injection)
- Quarantined (physical transient, 09-04 near-spill): 10 (excluded)
