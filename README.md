# ESP32-C6 SYSTIMER backstep: reproducer + mitigations

On **ESP32-C6 rev v0.2**, the SYSTIMER UNIT0 counter — the clock behind
`esp_timer_get_time()` — can spontaneously clear counter bits while running,
making `esp_timer` jump **backward by 15 minutes to 12 hours**. Verified at
the register level on two boards: after each event, UNIT0 equals UNIT1 (the
untouched OS-tick counter) with 1–6 specific bits zeroed, and no write path
was used. Requires Wi-Fi association plus an in-use I2C master; with Wi-Fi
modem sleep enabled it escalates to an Interrupt WDT panic in the Wi-Fi PHY
path.

## Is this your bug? (symptoms)

If any of these sound familiar, you are probably here for the right reason:

- **ESPHome/Home Assistant uptime stops incrementing** for 15 minutes to
  hours, then resumes counting from where it stopped — never catching up
- Device stays **online and connected** the whole time: API up, ping works,
  all entities still "available" — but every sensor value is **frozen**
- Timers, `interval:`, filters, and automations on the device stop firing,
  then all resume at once
- Recovers by itself after roughly 15/30/70/140 minutes (the duration is the
  size of the clock jump — often close to a power of two)
- On bare ESP-IDF: `esp_timer_get_time()` went backward /
  `Interrupt wdt timeout on CPU0` panic with a backtrace through
  `pp_timer_sleep_delay` → `esp_phy_disable` → `temp_sensor_get_raw_value`
- A task watchdog / `loop_watchdog` shows the FreeRTOS tick and `esp_timer`
  disagreeing by minutes
- Happens only on nodes that use **I2C** (any sensor, any driver) with Wi-Fi
  connected; your non-I2C C6 nodes are fine

Full evidence: [REPORT.md](REPORT.md).
Reported upstream: [espressif/esp-idf#19036](https://github.com/espressif/esp-idf/issues/19036).

**Updates since filing (2026-09-01):** two further events, bringing the total
to **12 bit-level-verified events** across two boards. One cleared **seven** bits
at once ({35,34,32,31,29,28,26}); one was captured in observation mode
(`REPRO_MODEM_SLEEP=0`) — no panic, and the device is still running with its
53.8-minute deficit intact, demonstrating the persistent-deficit behaviour
live. Observed range so far: bits 26–40 cleared, 1–7 per event, magnitudes 16.8 s–21.5 h (the small end is invisible at symptom level, so real-world rates are likely underestimated), always expressible as
all set bits within one contiguous span. The spans show structure: none of
the nine ever crosses the 35/36 boundary, clears are contiguous sub-ranges floating
within a segment (every [26,35]-region multi-bit event so far starts at 26;
a {40,37} clear left bit 36 set and surviving directly below it), and nothing below 26 has ever cleared (the 50 ms sampler
would catch even sub-second events). The 35/36 boundary is *witnessed*, not
inferred: in all four wide events, set bits at 36–39 sat directly above the
cleared span and survived untouched — and conversely one event cleared bit 36
alone while the populated [26,35] region below survived. Consistent with 10-bit counter
segments [26,35] / [36,45] losing state as a group, anchored at the
segment base. Notably these boundaries ignore the programmer-visible word
layout — the [26,35] spans straddle the counter's own VALUE_LO[31:0] /
VALUE_HI[19:0] split at bit 32 — so the grouping reflects structure inside
the counter core (e.g. timing-driven carry-pipeline segments), not anything
at the register, bus, or software level. Notably bit 26 is exactly the
52-bit counter's midpoint: the structure is consistent with a half-split
counter — a fast low 26-bit half that self-refreshes every tick (immune) and
a carry-fed high half that holds rare-toggle state (the vulnerable part),
with finer grouping inside the high half.

To be explicit about what "decomposes into cleared bits" means: after each
event, `UNIT0 == UNIT1 & ~mask` — the counter's value is bit-for-bit identical
to the unaffected reference counter except that 1–7 specific bits read zero.
Not "a backward jump that happens to factor": the surrounding bits are
untouched, which a borrow-propagating subtraction could not produce.

| directory | what it is |
|---|---|
| [`reproducer/`](reproducer/) | ~400-line bare ESP-IDF app that reproduces the fault in hours and prints/persists the exact bit-level evidence |
| [`components/clock_guard/`](components/clock_guard/) | ESPHome component: detects the backward jump from its own task and repairs the clock **in place** — six real events absorbed in production with zero downtime |
| [`components/loop_watchdog/`](components/loop_watchdog/) | ESPHome component: reboot watchdog for the frozen-scheduler state — the fail-safe layer behind clock_guard |

## Using the ESPHome components

```yaml
external_components:
  - source: github://danjurgens/esp32c6-systimer-backstep
    components: [clock_guard, loop_watchdog]
```

See each component's README for configuration and the recommended layering
(clock_guard repairs within ~1 s; loop_watchdog reboots at ~35 s if repair
ever fails — worst case equals watchdog-only behaviour).

## License

GPL-3.0-or-later. The ESPHome components link against ESPHome's GPLv3 C++
core, so this keeps everything compatible.
