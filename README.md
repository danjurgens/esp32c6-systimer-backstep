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
