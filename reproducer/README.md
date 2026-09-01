# ESP32-C6 SYSTIMER UNIT0 backstep reproducer

Minimal bare ESP-IDF app (~400 lines) reproducing a hardware fault on
ESP32-C6 rev v0.2: the SYSTIMER UNIT0 counter (used by `esp_timer`)
spontaneously clears one or more bits, making `esp_timer_get_time()` jump
backward by minutes to hours. With Wi-Fi modem sleep enabled this escalates to
an Interrupt WDT panic in the Wi-Fi PHY path.

See the accompanying bug report for the full evidence. Summary of the
conditions established there:

- Wi-Fi association AND an I2C master in use are both required.
- Neither alone reproduces (55.7 h Wi-Fi-off: zero events; non-I2C nodes:
  zero events over weeks).
- Events occur while the I2C bus is idle, not during transfers.
- Observed rate with both active: ~1 event per 7 h (2.0–11.2 h to first);
  a second board has gapped as long as ~76 h once.

**If you are trying to replicate:** a quiet hour means nothing. At the
observed rates, ~48 h of silence is where a negative becomes meaningful
(~1–2 % chance if your board were failing at ours' rate), and 72 h+ is a
solid negative worth reporting either way — a board that does NOT reproduce
is as useful to the investigation as one that does.

**The default build panics on an event** (`REPRO_MODEM_SLEEP=1` exercises
the Wi-Fi PHY hang), so a live console can scroll the evidence away in the
reboot. Nothing is lost: every event persists to NVS and the next boot
banner prints `boot #N | lifetime backsteps M` plus the last event's phase
and magnitude. Check that line — or build with `-DREPRO_MODEM_SLEEP=0` to
freeze-and-observe instead of crashing.

## Build

```
cp main/repro_secrets.h.example main/repro_secrets.h   # edit: your AP
idf.py set-target esp32c6
idf.py -DREPRO_WIFI=1 -DREPRO_I2C=1 -DREPRO_I2C_HZ=100000 \
       -DREPRO_INTERNAL_PULLUP=1 -DREPRO_BURST=3 \
       -DREPRO_IDLE_MS=1000 -DREPRO_IDLE_CHUNK_MS=50 build flash monitor
```

An I2C device must be attached (we used a VL53L1X at 0x29 on GPIO22/23; any
device that ACKs reads of 2 bytes works — the traffic pattern, not the
peripheral, is what matters).

## What it does

- Associates to Wi-Fi (`WIFI_PS_MIN_MODEM`, to also exercise the panic path).
- Drives 3 I2C transfers/s (3-transfer burst + 1000 ms idle — a ranging
  sensor's duty cycle).
- Samples `esp_timer` around every transfer and every 50 ms of bus idle, and
  compares consecutive readings (a backward step is a hardware fault —
  `esp_timer` is documented monotonic).
- On an event, prints the phase, the exact I2C transaction number, the raw
  UNIT0/UNIT1 counters, and persists everything to NVS so a panic-reboot
  cannot lose it. The last event is replayed on the next boot banner.

## Expected output on an event

```
E (...) repro: *** BACKSTEP DURING IDLE (idx 8) ***
E (...) repro:     esp_timer 4518998282 -> 224080988 us  (moved -4294917294 us = -71.58 min)
E (...) repro:     I2C SEQ: ok=13530 err=0 burst=4510
E (...) repro:     tick=4519017 ms  UNIT0=3585451554 UNIT1=72304272029  CONF=0xf7400002
Guru Meditation Error: Core  0 panic'ed (Interrupt wdt timeout on CPU0).
```

`UNIT1 - UNIT0` decomposes into the exact value of 1–7 cleared counter bits (i.e. UNIT0 equals UNIT1 with those bits zeroed);
compare the two values bitwise to see which.

## Build flags

| flag | default | meaning |
|---|---|---|
| `REPRO_WIFI` | 1 | associate to Wi-Fi (0 = control: does not reproduce) |
| `REPRO_I2C` | 1 | drive the I2C load (0 = control) |
| `REPRO_I2C_HZ` | 100000 | bus speed |
| `REPRO_INTERNAL_PULLUP` | 0 | 1 = internal ~45k pull-ups only |
| `REPRO_BURST` / `REPRO_IDLE_MS` | 3 / 1000 | duty pattern |
| `REPRO_IDLE_CHUNK_MS` | 50 | esp_timer sampling granularity across idle |
| `REPRO_MODEM_SLEEP` | 1 | 1 = `WIFI_PS_MIN_MODEM` (demonstrates the IWDT panic); 0 = `WIFI_PS_NONE` (observation mode: events freeze instead of crashing, device stays up, recovery observable) |

`CONFIG_ESP_DEBUG_OCDAWARE=n` is set in sdkconfig.defaults so a panic prints
its backtrace and reboots instead of halting for a debugger.
