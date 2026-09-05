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

**Second manifestation (2026-09-02):** the counter's snapshot handshake can
also wedge — `systimer_hal_get_counter_value()`'s unbounded
`while (!VALUE_VALID);` spin (executed from ROM on the C6) never returns, so
any `esp_timer_get_time()` caller hangs until the Interrupt WDT fires. Caught
with a full register dump: A5 = 0x6000a000 (SYSTIMER base), RA =
`esp_timer_impl_get_time`. This freezes the reader, so no backstep gets
recorded — a bounded retry + UPDATE reissue in the HAL would make it
recoverable in software.

**Largest natural event (2026-09-03):** a production node 63 hours into a
boot took a three-bit clear {41,40,38} — a 62.04 h backward jump (measured
223,338,300 ms vs predicted 223,338,299.4 — residual +0.6 ms). Its repair
component initially *refused* the correction as implausible under a fixed
2 h sanity cap and the reboot watchdog recovered the node instead; the cap
is now a physical plausibility bound (a bit-clear deficit can never exceed
the boot's tick uptime), which repairs events this large in place — validated
two days later when a 31 h clear {40,39,37} was absorbed with zero downtime.

**Updates since filing (2026-09-01):** two further events, bringing the total
to **21 bit-level-verified events** across two boards. One cleared **seven** bits
at once ({35,34,32,31,29,28,26}); one was captured in observation mode
(`REPRO_MODEM_SLEEP=0`) — no panic, and the device is still running with its
53.8-minute deficit intact, demonstrating the persistent-deficit behaviour
live. Observed range so far: bits 26–49 cleared, 1–6 bits per event, magnitudes
8.4 s to 492 days.

**Revision (event 15):** with the counter deliberately preset to dense
high-bit content, **clears are scattered subsets, not contiguous ranges** —
one event cleared bits {49,46,45,43,41,40} while set bits 42, 44, 47 and 48
*inside that same span* survived untouched. Fourteen earlier events looked
contiguous only because the gaps in their patterns held zeros, which cannot
testify; the first event with interior witnesses refuted contiguity outright.

What survives, now at 15/15 events:

- **Nothing below bit 26 ever clears.** Bit 26 is exactly the 52-bit
  counter's midpoint — consistent with a half-split counter whose low half
  increments (and thereby refreshes) every tick and is immune, while the
  carry-fed high half holds rare-toggle state and is the vulnerable part.
- **No event has ever mixed bits from both sides of the 35/36 line**, with
  set survivors witnessed on both sides of it repeatedly. Whether this is a
  hard structural edge or an artifact of event-window placement is open; a
  single mixed event would settle it.
- The affected structure ignores the programmer-visible word layout entirely
  (patterns straddle the VALUE_LO[31:0]/VALUE_HI[19:0] register split at bit
  32, and witnesses inside nibbles/bytes survive), so the grouping reflects
  structure inside the counter core, not anything at the register, bus, or
  software level.

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
