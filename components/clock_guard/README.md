# clock_guard — repair a backward-jumping esp_timer in place (ESPHome, ESP32)

On ESP32-C6 rev v0.2 the SYSTIMER UNIT0 counter (the clock behind
`esp_timer_get_time()` and therefore ESPHome's scheduler) can spontaneously
clear counter bits, jumping `esp_timer` backward by minutes to hours. The
scheduler then freezes for the duration of the jump: uptime flatlines while
the node stays online and every entity remains "available".

This component watches the divergence between the FreeRTOS tick (SYSTIMER
UNIT1, unaffected) and `esp_timer`, from its **own FreeRTOS task** — it cannot
be frozen by the fault it repairs. When `tick − esp_timer` exceeds the
threshold it calls `esp_timer_private_advance()` — the same call ESP-IDF
itself uses to re-sync `esp_timer` after light sleep — restoring the clock in
place. **No reboot, no gap**: on our production node six real events
(3.7–38.5 min) were absorbed with the 60 s publish cadence never missing a
beat.

## Usage

```yaml
external_components:
  - source:
      # published form:
      # - source: github://danjurgens/esp32c6-systimer-backstep
    components: [clock_guard]

clock_guard:
  check_interval: 1s   # must beat your reboot-watchdog's timeout (see below)
  threshold: 5s        # real events are minutes+; this only clears tick jitter
  max_correction: 2h   # refuse anything larger (more likely a bug than a fault)
  correction_count:
    name: "Clock Corrections"
  last_correction:
    name: "Last Clock Correction"
  total_corrected:
    name: "Total Clock Corrected"
```

## Layering (recommended)

Run this **in front of** a reboot watchdog such as `loop_watchdog`, and leave
the watchdog untouched: if the repair works the watchdog never sees enough
divergence to trip; if the repair ever fails, the watchdog reboots exactly as
before. Worst case equals the watchdog-only behaviour.

## Design notes

- Corrects **forward only**. Negative divergence means the tick lost time
  (missed interrupts, ~200 ms observed after long critical sections);
  advancing esp_timer then would be wrong, so the baseline is re-anchored
  instead.
- Counters and totals persist in NVS across reboots.
- On each correction it logs the raw pre-repair UNIT0/UNIT1 registers, which
  is how the fault's bit-clear signature was verified (ESP32-C6 addresses;
  harmless elsewhere, but the diagnostic logging is C6-specific).
- Known limit: with Wi-Fi **modem sleep** enabled the fault can hang the
  Wi-Fi PHY faster than any poll can win (Interrupt WDT panic). This
  component pairs best with `power_save_mode: none`; the panic-reboot path
  still recovers otherwise.
