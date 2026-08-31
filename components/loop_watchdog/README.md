# loop_watchdog — reboot out of a frozen ESPHome scheduler (ESP32)

Detects the state where the ESPHome scheduler stops (uptime flatlines, timers
stop firing) while the node stays online and connected — and reboots out of
it. Built against a hardware fault on ESP32-C6 rev v0.2 in which `esp_timer`
jumps backward by minutes to hours, freezing everything scheduled on it, but
it guards any scheduler-stall of this shape.

It runs in its **own FreeRTOS task** (a frozen scheduler cannot starve it) and
requires two independent signals before rebooting:

1. the main loop's heartbeat has stopped for `timeout`, and
2. (optional, default on) the tick clock and `esp_timer` have diverged past
   `clock_stall_threshold` — so a busy-but-alive loop is never mistaken for
   the fault.

A tick-clock-only backstop (`reboot_after`) reboots unconditionally after a
long stall even if the clock comparison is somehow unavailable.

On our production node it turned 20–40 minute presence-detection freezes into
~96-second outages, three for three, before being superseded by in-place
repair (`clock_guard`) — we now run both, this one as the safety net.

## Usage

```yaml
external_components:
  - source:
      type: local          # or github://... once published
      path: components
    components: [loop_watchdog]

loop_watchdog:
  update_interval: 10s        # heartbeat cadence from the main loop
  timeout: 35s                # no heartbeat this long -> stalled
  reboot: true
  require_clock_stall: true   # also require esp_timer divergence
  clock_stall_threshold: 10s
  reboot_after: 10min         # unconditional tick-clock backstop
  stall_count:
    name: "Scheduler Stalls"
  reboot_count:
    name: "Scheduler Stall Reboots"
  clock_divergence:
    name: "Clock Divergence"
  tick_uptime:
    name: "Tick Uptime"       # keeps counting when normal uptime freezes:
                              # graph both, a stall is the gap between them
  last_stall_duration:
    name: "Last Stall Duration"
  last_stall_divergence:
    name: "Last Stall Divergence"
```

Counters persist in NVS. A `test_trip()` self-test method is exposed for
wiring to a button — it fakes the full detection path (synthetic divergence,
withheld heartbeat) so you can verify the reboot and your alerting end to end;
a self-test is identifiable afterward by `last_stall_divergence` being exactly
2× `clock_stall_threshold`.

## Sibling component

`clock_guard` repairs the clock in place (no reboot) and is the better
day-to-day mitigation; run it in front of this component and keep this as the
fail-safe.
