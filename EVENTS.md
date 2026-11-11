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

### Update 2026-09-06 17:40

GA-13 (correction #19): {35,32,29} span(29,35) 40.82min, no cross, no survivors. 37 verified (24 TB + 13 GA).
Garage 19 corrections, 0 crossings, 0 unplanned downtime.

### Update 2026-09-05 20:14 (post-audit)

GA-12 (correction #18): {35,32,31,29,26} span(26,35) 43.13min, no cross,
no interior survivors. Garage now 18 corrections, 12 raw-captured/verified.
Bit-verified total: 36 (24 TB + 12 GA). 35/36 crossings: still 0.

## Totals

- **Bit-level-verified: 35** (24 testbed + 11 garage)
- Garage magnitude-only (raw lost): 6; non-correction (1 refused 62h + 3 pre-clock_guard): 4
- **Grand total faults across both boards: ~45 (45)**
- **35/36 crossings: 0** (2 apparent crossings during the 09-05 audit were artifacts: one my cumulative-deficit decomposition bug, one a leaked synthetic alert-test injection)
- Quarantined (physical transient, 09-04 near-spill): 10 (excluded)

## Boards and event rates

All three of our boards are ESP32-C6 v0.2. MACs are kept out of this repo, so
the boards are labelled here.

| label | board | role | conditions | observed rate |
|--|--|--|--|--|
| **XIAO-T** | Seeed XIAO ESP32-C6 (C6FH4, QFN32) | testbed: bare ESP-IDF reproducer (the TB-xx events above) | Wi-Fi STA + VL53L1X I2C bursts. PS_NONE and MIN_MODEM both used; events under both | **~7 h/event** (8 events in ~59 h of Wi-Fi runtime, REPORT.md); 0 in 55.7 h with Wi-Fi off |
| **XIAO-G** | Seeed XIAO ESP32-C6 (C6FH4, QFN32) | garage production node: ESPHome + clock_guard (the GA-xx events above) | Wi-Fi PS_NONE, VL53L1X on I2C | **~21 h/event**: 25 clock_guard corrections from 2026-08-27 to 09-18, and 2 more before it was retired around 09-19 |
| **DevKit** | Espressif ESP32-C6-DevKitC-1 (QFN40) | replaced XIAO-G in the garage | same reproducer on GPIO 6/7 | 0 events in ~5 days (2026-09-17 to 09-22) |

XIAO-G came off the garage breadboard and is the board in the November canary
run below. XIAO-T has the established ~7 h rate and isn't running right now.

## Canary run, November 2026 (XIAO-G, bare ESP-IDF reproducer)

Build: IDF v5.5.5. `REPRO_WIFI=1`, `REPRO_MODEM_SLEEP=0` (PS_NONE), I2C at 100 kHz in bursts of 3/s with 1000 ms idle
and `REPRO_LOAD_CANARY=1`. Internal pull-ups from boot #21; the GPTimer witness (`REPRO_WITNESS=1`) from boot #26.

Monitored runtime: **156.5 h** over four boots (6.0 h, 27.1 h, 1.5 h, and 121.9 h at 2026-11-10 22:30, still
running). UNIT1 losses of 15 ms or more were detectable throughout via the heartbeat deficit, and directly by the
witness from boot #26.

| # | when | unit | magnitude | detail |
|--|--|--|--|--|
| CN-01 | 2026-11-05, boot #23, 25.7 h | UNIT0 | 37.40 min (-2243.90 s) | Cleared bits {35, 30, 28, 27, 26}. The cleared group reads 0x000, not the canary's 0x169, and LOAD0 still holds the canary, so this was a clear, not a spurious load. Happened in the I2C idle gap with no transfer in flight. UNIT1 unaffected. |
| CN-02 | 2026-11-07, boot #26, 45.2 h | UNIT1 | ~35.7 ms | Witness: tick +250 ms against GPTimer +285.3 ms. UNIT0 stayed within 1 ms of the GPTimer; canary intact. The 250 ms poll can't tell a stall from a clear of low bits. |

That's **1 UNIT0 event in 156.5 h**. At XIAO-G's production rate (~21 h) about 7 were expected, and seeing 1 or
fewer has a probability of roughly 0.5%. Possible contributors include the instrumentation (the canary in the
load registers, and the witness's 250 ms critical-section sample) and run-to-run variation. Running the same build
on XIAO-T would separate the instrumentation from the board.
