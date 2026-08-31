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
namespace clock_guard {

// Architecture note (why this is a SEPARATE component from loop_watchdog):
//
//   loop_watchdog is the ultimate safety net: if the ESPHome scheduler stops
//   for `timeout`, it reboots. It is proven -- 3 real saves out of 3 -- and it
//   is deliberately NOT modified here.
//
//   clock_guard sits in front of it. The ESP32-C6 rev0.2 SYSTIMER UNIT0
//   (esp_timer) can step BACKWARD by tens of minutes (a silicon fault; nothing
//   writes the counter -- see FINDINGS 33/37). ESPHome's scheduler compares
//   deadlines against millis_64() = esp_timer, so a backward step freezes every
//   scheduled item for the duration of the jump.
//
//   The FreeRTOS tick (SYSTIMER UNIT1) is unaffected, so the divergence
//   tick - esp_timer is a direct measurement of how much esp_timer lost. IDF
//   already provides the repair: esp_timer_private_advance(), the same call the
//   OS uses to re-sync esp_timer after light sleep. Advancing by the measured
//   deficit restores the scheduler immediately.
//
//   If clock_guard corrects within its check_interval (default 1 s), the
//   divergence never reaches loop_watchdog's threshold and no reboot happens.
//   If it fails for any reason, loop_watchdog reboots as before. The fallback
//   is unchanged, so the worst case is exactly today's behaviour.
//
//   MUST run in its own task. As a PollingComponent it would be frozen by the
//   very fault it exists to repair.

class ClockGuard : public Component {
 public:
  void set_check_interval_ms(uint32_t v) { this->check_interval_ms_ = v; }
  void set_threshold_ms(uint32_t v) { this->threshold_ms_ = v; }
  void set_max_correction_ms(uint32_t v) { this->max_correction_ms_ = v; }
  void set_correction_count_sensor(sensor::Sensor *s) { this->count_sensor_ = s; }
  void set_last_correction_sensor(sensor::Sensor *s) { this->last_sensor_ = s; }
  void set_total_corrected_sensor(sensor::Sensor *s) { this->total_sensor_ = s; }
  void set_divergence_sensor(sensor::Sensor *s) { this->divergence_sensor_ = s; }

  void setup() override;
  void loop() override;          // publishes only; never repairs
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  static void task_trampoline(void *arg);
  void task_loop();              // own task -- runs even when the scheduler is frozen

 protected:
  int32_t divergence_ms_();
  void apply_correction_(int32_t excess_ms);
  void nvs_persist_();
  void nvs_restore_();

  uint32_t check_interval_ms_{1000};
  uint32_t threshold_ms_{5000};        // ignore anything smaller than this
  uint32_t max_correction_ms_{7200000};  // 2 h sanity cap

  int32_t baseline_ms_{0};
  bool have_baseline_{false};

  // written by the task, read by loop() for publishing
  volatile uint32_t corrections_{0};
  volatile int32_t last_correction_ms_{0};
  volatile int64_t total_corrected_ms_{0};
  volatile int32_t last_divergence_ms_{0};
  volatile bool dirty_{false};
  volatile uint32_t refused_{0};       // corrections rejected by the sanity cap

  sensor::Sensor *count_sensor_{nullptr};
  sensor::Sensor *last_sensor_{nullptr};
  sensor::Sensor *total_sensor_{nullptr};
  sensor::Sensor *divergence_sensor_{nullptr};

  TaskHandle_t task_handle_{nullptr};
};

}  // namespace clock_guard
}  // namespace esphome
