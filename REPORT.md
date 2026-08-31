# ESP32-C6 rev v0.2: SYSTIMER UNIT0 spontaneously clears counter bits

**Status:** draft for review. Not yet submitted.
**Suggested venue:** github.com/espressif/esp-idf issues, label `chip:esp32c6`.

---

## Title

> ESP32-C6 v0.2: SYSTIMER UNIT0 spontaneously clears counter bits — esp_timer
> jumps backward minutes to hours (Wi-Fi + I2C)

---

## Findings

1. **The SYSTIMER UNIT0 counter (used by `esp_timer`) spontaneously clears one
   or more bits while running.** After each event, `UNIT0 == UNIT1 & ~mask`:
   every cleared bit is verified to have been set in UNIT1 (the OS-tick
   counter, which shares the clock and is never affected) — bit-for-bit, on
   all **seven** fully-instrumented events, across **both boards** and both
   power-save modes. Bits 26–39 observed, up to **six** at once. Every event
   is additionally consistent with a stronger form: **all set bits within one
   contiguous range [lo,hi] zeroed** (widths 1–9), as if a segment of the
   counter register loses state. Jumps range 15 minutes to 12 hours and are
   permanent for the life of the boot.

2. **No write path was used.** The sticky `SYSTIMER_UNIT0_LOAD_HI/LO` registers
   and the LOAD commit bit read zero during live events (JTAG, CPU not halted).
   Every SYSTIMER-writing call site in IDF v5.5.5 was enumerated; none is
   reachable in this configuration.

3. **The counter is otherwise healthy.** Both WORK_EN bits stay set; UNIT0
   advances at the correct rate immediately before and after the step. It loses
   the value of the cleared bits in a single step, nothing else.

4. **Wi-Fi AND I2C are both required; neither alone reproduces.**
   - *I2C without Wi-Fi:* the same board, same I2C load, `REPRO_WIFI=0` —
     **55.7 h / 601k transactions, zero events.**
   - *Wi-Fi without I2C:* three other C6 nodes on the same network (one with
     `WIFI_PS_MIN_MODEM` **and** continuous active Wi-Fi scanning — far more
     radio/PMU activity than the failing nodes) — **zero events across weeks
     of cumulative uptime.** Across the 5-node fleet, only the two nodes
     running an I2C master have ever faulted.
   - *Both:* 8 events in ~59 h on the test board (mean ≈ 7 h).

   Modem sleep is **not** part of the required condition: the production node
   runs `power_save_mode: none` and faults regardless (events observed under
   both `WIFI_PS_NONE` and `WIFI_PS_MIN_MODEM`). Wi-Fi association is the
   co-factor; the power-save mode only selects the consequence (finding 6).

5. **Every event occurred while the I2C bus was idle** (≥350 ms after the last
   transfer; n=6) — not during transfers and not at the idle→active
   transition. Combined with finding 4: having the I2C master configured and
   in use is required, but concurrent bus traffic is not.

6. **Consequence depends on power-save mode — modem sleep is a crash path,
   not a cause.** With `WIFI_PS_MIN_MODEM`, the backward clock reaches
   `pp_timer_sleep_delay` and the PHY shutdown path hangs →
   `Interrupt wdt timeout on CPU0`. With `WIFI_PS_NONE` (our production
   node) the same corruption occurs but "only" freezes everything scheduled
   on `esp_timer` for the duration of the jump.

Two physical boards (Seeed XIAO ESP32-C6, both rev v0.2), reproduced under
ESPHome and under a ~350-line bare ESP-IDF app. A production node running our
measure-and-correct workaround has absorbed six events with zero downtime.

---

## Environment

| | |
|---|---|
| Chip | ESP32-C6, **rev v0.2** (`efuse_init: Chip rev: v0.2`) |
| Board | Seeed Studio XIAO ESP32-C6 (×2, both affected) |
| ESP-IDF | **v5.5.5** (also seen on the ESPHome 2026.8.1 toolchain) |
| Config | Unicore, 160 MHz, `CONFIG_PM_ENABLE=n`, `CONFIG_FREERTOS_HZ=1000` |
| SYSTIMER | fixed /2.5 divider from 40 MHz XTAL = **16 MHz** (`SOC_SYSTIMER_FIXED_DIVIDER`) |
| Wi-Fi | events under both `WIFI_PS_NONE` and `WIFI_PS_MIN_MODEM` (reproducer uses `MIN_MODEM` to exercise the panic path) |
| Peripheral load | I2C master, 100 kHz, internal pull-ups, 3 transfers/s |

---

## Evidence: UNIT0 is UNIT1 with bits cleared

How the asserted bit-clears are derived, and how they are verified — two
separate steps:

**Derivation.** UNIT1 (the OS-tick counter) shares UNIT0's clock and normally
tracks it to within a few ms, so `D = UNIT1 − UNIT0` measures what UNIT0 lost.
We greedily decompose D into powers of two with ±50 ms tolerance. Any integer
decomposes into powers of two, so this step alone proves nothing — its only
signal is **sparsity**: every event needs just 1–4 terms, where an arbitrary
value of this magnitude would need ~20.

| event | jump | derived bits | detected (ms into bus idle) |
|---|---|---|---|
| 1 | 715.8 min | 2^39 + 2^37 | 400–450 |
| 2 | 71.6 min | 2^36 | 400–450 |
| 3 | 27.1 min | 2^34 + 2^33 + 2^27 + 2^26 | 900–950 |
| 4 | 143.2 min | 2^37 | 350–400 |
| 5 | 71.6 min | 2^36 | 900–950 |
| 6 | 429.5 min | 2^38 + 2^37 | 450–500 |
| 7* | 15.3 min | 2^33 + 2^32 + 2^30 + 2^29 + 2^27 + 2^26 | n/a (production node) |

(The reproducer samples `esp_timer` in 50 ms slices across its 1000 ms I2C
idle; the last column is the slice in which the backstep was detected. Event
7* is from the second, production board — captured by its 1 Hz repair task
before correcting, so it has no sub-second timing; its residual is −2.0 ms,
matching that node's healthy baseline divergence exactly. In
every event the decomposition's leftover is a constant +40.3 to +41.0 ms:
the two raw registers are latched at slightly different moments in the capture
path, with UNIT0's snapshot ~41 ms fresher — fixed measurement skew, which
does not scale across masks spanning three orders of magnitude.)

**Verification (independent of the arithmetic).** We then compare the two raw
counter values **bit by bit**. On all six events, at every bit position above
22, UNIT0 and UNIT1 are *identical* — except at exactly the derived positions,
where UNIT1 holds 1 and UNIT0 holds 0:

| event | bits differing above 22 | derived bits | match |
|---|---|---|---|
| 1 | 37, 39 | 39, 37 | exact |
| 2 | 36 | 36 | exact |
| 3 | 26, 27, 33, 34 | 34, 33, 27, 26 | exact |
| 4 | 37 | 37 | exact |
| 5 | 36 | 36 | exact |
| 6 | 37, 38 | 38, 37 | exact |
| 7* | 26, 27, 29, 30, 32, 33 | 33, 32, 30, 29, 27, 26 | exact |

Differences at and below bit 22 (< 0.5 s of count) are the ~41 ms snapshot
snapshot skew plus its carries; the smallest derived bit is 26, leaving
a clean separation gap at bits 23–25 in every event.

This is what rules out "a backward jump that merely factors nicely":
subtracting an arbitrary sparse value from UNIT1 would propagate **borrows**
through the lower bits, scrambling them. Instead the lower bits (above the
skew) are untouched. UNIT0 is literally UNIT1 with 1–4 specific bits cleared:
`UNIT0 == UNIT1 & ~mask`, up to snapshot skew.

One further structural observation: in every event the cleared bits are **all
of UNIT1's set bits within one contiguous range** — e.g. event 7 cleared the
six set bits in [26,33] while bits 28 and 31, zero in UNIT1, sat untouched
between them. This cannot be distinguished from independent per-bit clears
(zero bits are unobservable), but no event refutes it, and it would point at a
contiguous segment of the counter register losing state.

Raw capture of one event:

```
*** BACKSTEP DURING IDLE (idx 8) ***
    esp_timer 4518998282 -> 224080988 us   (moved -4294917294 us = -71.58 min)
    I2C SEQ: ok=13530 err=0 burst=4510
    tick=4519017 ms  UNIT0=3585451554 UNIT1=72304272029  CONF=0xf7400002
```

### Both counters remain enabled and correctly clocked

`SYSTIMER_CONF = 0xf7400002` throughout: `TIMER_UNIT0_WORK_EN` (bit 30) and
`TIMER_UNIT1_WORK_EN` (bit 29) both set. Sampling UNIT0 over a stall shows it
advancing at **1.009 s/s** — the correct rate. The counter is not stopped, not
slow, and not drifting. It loses exactly the value of the cleared bits in one
step and then counts correctly again.

---

## No write path was used

`SYSTIMER_UNIT0_LOAD_HI` (`0x6000a00c`), `SYSTIMER_UNIT0_LOAD_LO`
(`0x6000a010`) and the `SYSTIMER_UNIT0_LOAD` commit bit (`0x6000a05c`) all read
**zero** during live events, verified over JTAG without halting the CPU.

These registers are sticky — they retain the last value written and are not
cleared by a load. Had any agent (CPU store, REGDMA, ROM, a driver) loaded the
counter, UNIT0 would equal `{LOAD_HI, LOAD_LO}` = 0. It does not; it sits at its
corrupted-but-large value.

We also enumerated every caller in IDF v5.5.5 that can write a SYSTIMER counter:

| site | counter | reachable here? |
|---|---|---|
| `port_systick.c:87,110` | UNIT1 | init + dual-core stagger only |
| `esp_timer_impl_systimer.c:138` (`esp_timer_impl_set`) | UNIT0 | only via `esp_timer_private_set` |
| `esp_timer_impl_systimer.c:146` (`esp_timer_impl_advance`) | UNIT0 | **no production callers** |

`esp_timer_private_set` has exactly one production caller —
`sleep_modes.c:1626`, the light-sleep wake resync — which is unreachable with
`CONFIG_PM_ENABLE=n`. REGDMA peripheral retention
(`port/esp32c6/system_periph_retention.c`) does write the counters, but is armed
only for sleep with peripheral power-down, and restores **both** units, so it
cannot produce a UNIT0-only deficit.

---

## Downstream failure: Interrupt WDT in the Wi-Fi PHY modem-sleep path

With `CONFIG_ESP_DEBUG_OCDAWARE=n`, the panic prints a usable backtrace:

```
Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout on CPU0).
MEPC : 0x40017600   RA : 0x40806f10   MCAUSE : 0x18

temp_sensor_get_raw_value   components/esp_hw_support/sar_periph_ctrl_common.c:119
phy_get_tsens_value         components/esp_phy/src/phy_override.c:102
esp_phy_disable             components/esp_phy/src/phy_init.c:439
esp_phy_disable_wrapper     components/esp_wifi/esp32c6/esp_adapter.c:568
pp_timer_sleep_delay        pp_timer.o
vPortTaskWrapper
```

`MEPC` is in ROM. `0x69666977` ("wifi") appears on the stack.

In **2 of 8 panics** the device hung before any poll observed the step — no
firmware detector (250 ms and 50 ms sampling) recorded a backstep and the
NVS event counter did not increment, yet the crash signature is identical. The
corruption can evidently land while the sleep-timing computation is already in
progress, engaging the hang faster than any sampler.

Sequence: the counter loses 2^N ticks → `pp_timer_sleep_delay` computes
modem-sleep timing from a clock that has moved backward by minutes-to-hours →
`esp_phy_disable` → `phy_get_tsens_value` → `temp_sensor_get_raw_value` does not
complete → Interrupt WDT fires.

**Without Wi-Fi power save the same corruption is far less damaging** (without
Wi-Fi entirely it does not occur at all — finding 4): with `WIFI_PS_NONE` the
sleep path above is never taken, and the event merely freezes anything
scheduled on `esp_timer` until the counter climbs back past the pending
deadlines. Our production node runs `WIFI_PS_NONE`; its symptom across every
event has been a scheduler freeze whose duration equals the jump, never a
panic.

---

## Timing correlation: always during bus idle

The reproducer performs 3 I2C transfers, then idles 1000 ms, sampling
`esp_timer` every 50 ms across the idle. **All six events occurred during the
idle window** — never inside a transfer, never between transfers, never at the
idle->active transition.

The detection offset varies (350–500 ms for four events, 900–950 ms for two),
so there is no preferred position within the idle window; the only consistent fact is that the bus had been quiet for at least
350 ms. We had hypothesised the idle->active transition as the trigger; six
events refute it.

## Minimal reproducer

`idf-repro/` in the attached archive. ~350 lines, ESP-IDF v5.5.5, no external
components. Build flags:

```
idf.py -DREPRO_WIFI=1 -DREPRO_I2C=1 -DREPRO_I2C_HZ=100000 \
       -DREPRO_INTERNAL_PULLUP=1 -DREPRO_BURST=3 \
       -DREPRO_IDLE_MS=1000 -DREPRO_IDLE_CHUNK_MS=50 build
```

It associates to Wi-Fi with `WIFI_PS_MIN_MODEM`, drives a VL53L1X-pattern I2C
load at 3 transfers/s, samples `esp_timer` around every transfer and every 50 ms
of idle, and persists each event to NVS so a panic-reboot cannot lose it.

**Event rate:** 8 events in ~59 h of cumulative Wi-Fi-enabled runtime on one
board (mean ≈ 7 h; first event 2.8–10.7 h after boot across runs). The same
board ran 55.7 h with `REPRO_WIFI=0` and zero events.

---

## Already ruled out (so you can skip these)

| hypothesis | how it was excluded |
|---|---|
| Software wrote the counter | LOAD registers zero during live events; full caller enumeration |
| Light sleep resync (`sleep_modes.c:1626`) | `CONFIG_PM_ENABLE=n`; path not linked |
| REGDMA retention restore | armed only for sleep w/ power-down; restores both units |
| SNTP / `settimeofday` | adjusts a software boot-time offset only; cannot touch the counter |
| Memory corruption / wild pointer | would have to hit 3 registers in order; SRAM and peripherals are 0.5 GB apart; zero crashes or MPU faults across 21.5 M transactions |
| I2C traffic alone | 55.7 h / 601,005 transactions with Wi-Fi off: **zero events** |
| Wi-Fi power save alone | a non-I2C node with `power_save_mode: LIGHT` and continuous active scanning has never faulted |
| Counter stopped or slow | UNIT0 advances at 1.009 s/s during the deficit; both WORK_EN bits set |
| Board-specific defect | two boards, both affected |
| ESPHome / framework | reproduced in bare ESP-IDF with no framework |
| The VL53L1X sensor or its driver | the same signature was observed earlier with a **different sensor** (RCWL-1601 ultrasonic, I2C mode) on **ESPHome's stock main-loop `i2c:` driver**: uptime stalls, then resumes counting from where it stopped — not jumping to where it should be — leaving it permanently behind wall clock. That is the esp_timer freeze fingerprint (ESPHome's uptime sensor publishes esp_timer). Two sensors, two drivers, one signature |
| Bus speed / pull-ups as root cause | the mechanism is identical under internal-pull-up/100 kHz and external-4.7k/50 kHz; observed rates differ only ~2× between them, within the 4–5× week-to-week swing seen on a fixed configuration, so any rate effect is unresolved |

---

## Questions for Espressif

1. Is this a known erratum for ESP32-C6 rev v0.2? The published errata list we
   found does not appear to cover SYSTIMER counter corruption.
2. Is there a documented condition under which SYSTIMER counter bits can be
   lost — a clock-domain crossing, an APB/PLL transition, or an interaction with
   modem-sleep RF power cycling?
3. `esp_timer` is documented as monotonic and much of IDF relies on that.
   Should `esp_timer_impl_get_time()` sanity-check against the OS tick counter,
   or should `pp_timer_sleep_delay` be defensive about non-monotonic input? The
   backward step is a hardware fault, but the *hang* is a software consequence
   that a bounds check would turn into a recoverable glitch.
4. Is rev v0.3+ affected?

---

## Mitigation we are running (for others who hit this)

Because UNIT1 is unaffected, `tick - esp_timer` measures the deficit directly,
and `esp_timer_private_advance()` — IDF's own re-sync call — puts it back. A
1 Hz task doing exactly that has absorbed **six** real events on the production
node with no observable interruption: publish cadence stayed metronomic across
each, and uptime never reset.

This is a workaround, not a fix; it cannot help the first few hundred
milliseconds, and it does not prevent the Wi-Fi PHY hang if the corruption lands
inside that path.

---

## Not yet done — stated plainly

- The idle-only timing correlation is n=6, one board. Suggestive, not
  established.
- Two boards, one board type, likely one production batch.
- No temperature, voltage, or clock-source variation attempted.
- The contiguous-range interpretation cannot be distinguished from
  independent per-bit clears with this data (zero bits are unobservable).
