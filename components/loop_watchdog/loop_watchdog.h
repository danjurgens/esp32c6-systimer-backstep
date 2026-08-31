/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dan Jurgens
 * Part of https://github.com/danjurgens/esp32c6-systimer-backstep
 */
#pragma once

#include "esphome/core/component.h"
#include "esphome/components/sensor/sensor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace esphome {
namespace loop_watchdog {

// Watchdog and recorder for a STALLED ESPHome SCHEDULER.
//
// The failure (captured live over JTAG on an ESP32-C6, see ../FINDINGS.md):
// every scheduled callback stops -- PollingComponent::update(), interval:,
// set_timeout/set_interval -- while loopTask keeps looping, WiFi stays up, the
// API stays connected, and Home Assistant keeps every entity "available" while
// showing values frozen minutes or hours earlier. On the testbed the scheduler
// clock (esp_timer, via millis_64) had stopped while the FreeRTOS tick kept
// running; that node never recovered. A second node showed a ~20 minute stall
// that recovered on its own with no reboot.
//
// Nothing in ESPHome catches this: api:/wifi: reboot_timeout never fire (the
// API never drops, and those timers are scheduler-driven anyway), the task
// watchdog is fed by a healthy loopTask, and a component's own "healthy"
// sensor is published from inside update() -- the thing that stopped -- so it
// freezes in its last state instead of going unhealthy.
//
// So this component watches from OUTSIDE the scheduler, in its own FreeRTOS
// task driven by ticks. It does two jobs:
//   1. RECORD every stall, including ones that recover by themselves, and
//      report them to HA once the scheduler comes back.
//   2. REBOOT only if the stall looks unrecoverable (see require_clock_stall).
class LoopWatchdog : public PollingComponent {
 public:
  void set_timeout_ms(uint32_t t) { this->timeout_ms_ = t; }
  void set_reboot(bool r) { this->reboot_ = r; }
  void set_require_clock_stall(bool r) { this->require_clock_stall_ = r; }
  void set_clock_stall_ms(uint32_t t) { this->clock_stall_ms_ = t; }
  void set_reboot_after_ms(uint32_t t) { this->reboot_after_ms_ = t; }
  void set_micro_stall_ms(uint32_t t) { this->micro_stall_ms_ = t; }
  void set_divergence_sensor(sensor::Sensor *s) { this->divergence_sensor_ = s; }
  void set_tick_uptime_sensor(sensor::Sensor *s) { this->tick_uptime_sensor_ = s; }
  void set_stall_count_sensor(sensor::Sensor *s) { this->stall_count_sensor_ = s; }
  void set_last_stall_duration_sensor(sensor::Sensor *s) { this->last_stall_duration_sensor_ = s; }
  void set_last_stall_divergence_sensor(sensor::Sensor *s) { this->last_stall_divergence_sensor_ = s; }
  void set_reboot_count_sensor(sensor::Sensor *s) { this->reboot_count_sensor_ = s; }
  void set_micro_stall_count_sensor(sensor::Sensor *s) { this->micro_stall_count_sensor_ = s; }
  void set_total_stalled_sensor(sensor::Sensor *s) { this->total_stalled_sensor_ = s; }

  // Deliberately trip the watchdog, to verify the recovery path END TO END
  // without waiting for a natural fault.
  //
  // The safety net has never been demonstrated: v1 sat through a real 24.6 min
  // freeze without acting, and v3 is untested code. "Trust it after a quiet
  // week" cannot distinguish a working watchdog from a lucky one -- only
  // seeing it catch something can.
  //
  // Simulates BOTH conditions the real fault produces: update() stops stamping
  // its heartbeat (starvation), and the clock appears to have jumped
  // (divergence). The task then takes the ordinary detection path, writes NVS,
  // and reboots -- exercising exactly the code a real stall would.
  void test_trip();

  void setup() override;
  void update() override;  // scheduler-driven: this IS the heartbeat
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  static void task_trampoline(void *arg);
  void task_loop();  // own FreeRTOS task -- survives a dead scheduler

 protected:
  // Signed (tick - esp_timer) in ms. Healthy: a few ms. A stalled esp_timer
  // makes this grow at ~1 ms per ms of real time.
  int32_t clock_divergence_ms_();

  // NVS-backed so a stall survives a reboot AND a power cycle. Written from
  // the watchdog task; ESPHome's preference machinery syncs on the scheduler,
  // which is exactly what is dead at that moment.
  void nvs_bump_(const char *key, int32_t value_key_val, bool has_value);
  // Add lost time to the lifetime total. Survives the reboots the watchdog
  // itself causes -- otherwise every trip resets the record to zero.
  void nvs_add_stalled_(uint32_t ms);
  // Catch stalls SHORTER than `timeout`, which never trip the watchdog but
  // still permanently offset esp_timer. Detected as a jump in divergence
  // between two consecutive update() calls.
  void check_micro_stall_(int32_t divergence_now);
  void load_and_publish_stats_();

  uint32_t timeout_ms_{120000};       // update() starvation before we call it a stall
  uint32_t clock_stall_ms_{30000};    // divergence that counts as a stalled clock
  uint32_t micro_stall_ms_{2000};     // divergence jump that counts as a micro-stall
  uint32_t reboot_after_ms_{600000};  // stall this long -> reboot even if the
                                      // clock looks fine (0 = never)
  bool reboot_{true};
  bool require_clock_stall_{true};

  // Written by update() on the main loop, read by the watchdog task. Tick-based
  // on purpose: it must not depend on the clock under suspicion.
  volatile uint32_t last_beat_ms_{0};
  volatile bool armed_{false};  // written by update(), read by the task

  // Stall bookkeeping, shared between the task and update().
  volatile bool in_stall_{false};
  volatile uint32_t stall_started_ms_{0};
  volatile int32_t stall_peak_divergence_{0};
  // Divergence as of the last time update() ran, i.e. the last moment the
  // scheduler was demonstrably alive. This is the BASELINE the task compares
  // against. Absolute divergence is useless for detection because the deficit
  // is CUMULATIVE and PERMANENT -- each backward step of SYSTIMER UNIT0 adds
  // to it forever, so after one stall the absolute value sits far above any
  // fixed threshold and would report "clock stalled" permanently.
  volatile int32_t healthy_divergence_ms_{0};
  volatile bool have_healthy_divergence_{false};
  volatile bool suppress_micro_check_{false};
  volatile bool test_trip_{false};
  volatile bool stall_ended_pending_{false};
  volatile uint32_t last_stall_duration_ms_{0};

  bool stats_published_{false};
  int32_t last_divergence_ms_{0};
  bool have_last_divergence_{false};

  sensor::Sensor *divergence_sensor_{nullptr};
  sensor::Sensor *tick_uptime_sensor_{nullptr};
  sensor::Sensor *stall_count_sensor_{nullptr};
  sensor::Sensor *last_stall_duration_sensor_{nullptr};
  sensor::Sensor *last_stall_divergence_sensor_{nullptr};
  sensor::Sensor *reboot_count_sensor_{nullptr};
  sensor::Sensor *micro_stall_count_sensor_{nullptr};
  sensor::Sensor *total_stalled_sensor_{nullptr};
  TaskHandle_t task_handle_{nullptr};
};

}  // namespace loop_watchdog
}  // namespace esphome
