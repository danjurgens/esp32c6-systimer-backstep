/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dan Jurgens
 * Part of https://github.com/danjurgens/esp32c6-systimer-backstep
 */
#include "loop_watchdog.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs.h"

namespace esphome {
namespace loop_watchdog {

static const char *const TAG = "loop_watchdog";

static const char *const NVS_NS = "loopwdt";
static const char *const KEY_STALLS = "stalls";   // total stalls seen
static const char *const KEY_REBOOTS = "reboots";  // stalls we rebooted on
static const char *const KEY_LASTDUR = "lastdur";  // last stall duration, ms
static const char *const KEY_LASTDIV = "lastdiv";  // divergence at last stall
static const char *const KEY_MICRO = "micro";      // stalls shorter than `timeout`
static const char *const KEY_TOTALMS = "totalms";  // lifetime esp_timer time lost

// millis() is tick-based (xTaskGetTickCount). esp_timer_get_time() is the clock
// the ESPHome scheduler actually runs on. Comparing them is the whole point.
int32_t LoopWatchdog::clock_divergence_ms_() {
  const uint32_t tick_ms = millis();
  const uint32_t esp_ms = (uint32_t) (esp_timer_get_time() / 1000);
  return (int32_t) (tick_ms - esp_ms);
}

// Increment a counter, optionally storing an associated value.
void LoopWatchdog::nvs_bump_(const char *key, int32_t value_key_val, bool has_value) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "NVS open failed; stall not persisted");
    return;
  }
  uint32_t n = 0;
  nvs_get_u32(h, key, &n);
  nvs_set_u32(h, key, n + 1);
  if (has_value) {
    nvs_set_i32(h, KEY_LASTDIV, value_key_val);
    nvs_set_u32(h, KEY_LASTDUR, (uint32_t) this->last_stall_duration_ms_);
  }
  nvs_commit(h);
  nvs_close(h);
}

void LoopWatchdog::nvs_add_stalled_(uint32_t ms) {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK)
    return;
  uint32_t total = 0;
  nvs_get_u32(h, KEY_TOTALMS, &total);
  nvs_set_u32(h, KEY_TOTALMS, total + ms);
  nvs_commit(h);
  nvs_close(h);
}

// Runs on the main loop from update(). A stall shorter than `timeout` never
// reaches the watchdog task's logic, but esp_timer still loses that time
// permanently -- so it shows up as a STEP in divergence between two
// consecutive update() calls. Without this, a drizzle of short stalls would be
// completely invisible, and only the rare long ones would ever be recorded.
void LoopWatchdog::check_micro_stall_(int32_t divergence_now) {
  if (!this->have_last_divergence_) {
    this->last_divergence_ms_ = divergence_now;
    this->have_last_divergence_ = true;
    return;
  }
  const int32_t step = divergence_now - this->last_divergence_ms_;
  this->last_divergence_ms_ = divergence_now;
  if (this->suppress_micro_check_) {
    this->suppress_micro_check_ = false;  // this step was a full stall, already counted
    return;
  }
  if (step > (int32_t) this->micro_stall_ms_) {
    ESP_LOGW(TAG, "MICRO-STALL: esp_timer lost %d ms since the last update() "
                  "(below the %u ms stall threshold, so no reboot)",
             (int) step, (unsigned) this->timeout_ms_);
    this->nvs_bump_(KEY_MICRO, step, false);
    this->nvs_add_stalled_((uint32_t) step);
  }
}

void LoopWatchdog::load_and_publish_stats_() {
  nvs_handle_t h;
  uint32_t stalls = 0, reboots = 0, lastdur = 0, micro = 0, totalms = 0;
  int32_t lastdiv = 0;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, KEY_STALLS, &stalls);
    nvs_get_u32(h, KEY_REBOOTS, &reboots);
    nvs_get_u32(h, KEY_LASTDUR, &lastdur);
    nvs_get_i32(h, KEY_LASTDIV, &lastdiv);
    nvs_get_u32(h, KEY_MICRO, &micro);
    nvs_get_u32(h, KEY_TOTALMS, &totalms);
    nvs_close(h);
  }
  if (stalls > 0)
    ESP_LOGW(TAG,
             "scheduler stall history: %u stall(s), %u ended in reboot; "
             "last lasted %u ms with divergence %d ms",
             (unsigned) stalls, (unsigned) reboots, (unsigned) lastdur, (int) lastdiv);
  else
    ESP_LOGI(TAG, "no scheduler stalls recorded on this device");

  if (this->stall_count_sensor_ != nullptr)
    this->stall_count_sensor_->publish_state((float) stalls);
  if (this->reboot_count_sensor_ != nullptr)
    this->reboot_count_sensor_->publish_state((float) reboots);
  if (this->last_stall_duration_sensor_ != nullptr)
    this->last_stall_duration_sensor_->publish_state((float) lastdur);
  if (this->last_stall_divergence_sensor_ != nullptr)
    this->last_stall_divergence_sensor_->publish_state((float) lastdiv);
  if (this->micro_stall_count_sensor_ != nullptr)
    this->micro_stall_count_sensor_->publish_state((float) micro);
  if (this->total_stalled_sensor_ != nullptr)
    this->total_stalled_sensor_->publish_state((float) totalms / 1000.0f);
}

// Self-test. Stops the heartbeat and skews the healthy baseline so the task's
// normal logic sees both starvation and a clock jump. Nothing is faked past
// that point: detection, thresholds, NVS accounting and the reboot are the
// real code paths.
void LoopWatchdog::test_trip() {
  ESP_LOGW(TAG, "SELF-TEST ARMED: heartbeat frozen and baseline skewed.");
  ESP_LOGW(TAG, "  expect a reboot in ~%u s, and the counters to increment by 1.",
           (unsigned) (this->timeout_ms_ / 1000));
  this->test_trip_ = true;
  /* Make the next divergence read look like a large backward jump. */
  this->healthy_divergence_ms_ = this->clock_divergence_ms_() - (int32_t) (this->clock_stall_ms_ * 2);
  this->have_healthy_divergence_ = true;
}

void LoopWatchdog::setup() {
  this->last_beat_ms_ = millis();
  xTaskCreate(&LoopWatchdog::task_trampoline, "loop_wdt", 4096, this, 4, &this->task_handle_);
  if (this->task_handle_ == nullptr) {
    ESP_LOGE(TAG, "watchdog task create failed - NOT protected");
    this->mark_failed();
  }
}

// Runs on the ESPHome scheduler. If the scheduler stalls this stops being
// called, which is what the task below detects. When it starts being called
// again, that is the stall ending -- and THAT is when we can finally report.
void LoopWatchdog::update() {
  if (this->test_trip_) {
    /* Deliberately do NOT stamp the heartbeat: the task must see starvation. */
    ESP_LOGW(TAG, "SELF-TEST: withholding heartbeat (%u ms starved)",
             (unsigned) (millis() - this->last_beat_ms_));
    return;
  }
  this->last_beat_ms_ = millis();
  this->armed_ = true;

  if (!this->stats_published_) {
    this->stats_published_ = true;
    this->load_and_publish_stats_();
  }

  // A stall that recovered on its own. The scheduler is alive again, so this
  // is the first moment we can persist and publish it. Without this branch a
  // self-recovering stall leaves no trace anywhere -- observed on a node that
  // stalled ~20 min and came back with no reboot.
  if (this->stall_ended_pending_) {
    this->stall_ended_pending_ = false;
    // The divergence step across a real stall must NOT also be counted as a
    // micro-stall; it is already recorded below as a stall.
    this->suppress_micro_check_ = true;
    ESP_LOGW(TAG, "scheduler RECOVERED after %u ms stall (peak divergence %d ms)",
             (unsigned) this->last_stall_duration_ms_, (int) this->stall_peak_divergence_);
    this->nvs_bump_(KEY_STALLS, this->stall_peak_divergence_, true);
    this->nvs_add_stalled_(this->last_stall_duration_ms_);
    this->load_and_publish_stats_();
  }

  const int32_t div_now = this->clock_divergence_ms_();
  // update() only runs when the scheduler is alive, so this IS a healthy sample.
  this->healthy_divergence_ms_ = div_now;
  this->have_healthy_divergence_ = true;
  this->check_micro_stall_(div_now);
  if (this->divergence_sensor_ != nullptr)
    this->divergence_sensor_->publish_state((float) div_now);

  // Tick-based uptime. ESPHome's own uptime sensor publishes millis_64(),
  // which is esp_timer -- the clock that freezes. This one is xTaskGetTickCount
  // and keeps running. Side by side in HA history, a stall shows up as the
  // esp_timer uptime going flat while this keeps climbing, and the step
  // between them after recovery IS the stall duration. No JTAG required.
  if (this->tick_uptime_sensor_ != nullptr)
    this->tick_uptime_sensor_->publish_state((float) millis() / 1000.0f);
}

void LoopWatchdog::task_trampoline(void *arg) { static_cast<LoopWatchdog *>(arg)->task_loop(); }

void LoopWatchdog::task_loop() {
  uint32_t last_report_ms = 0;

  // Proof of life. Without this an inert watchdog is indistinguishable from a
  // working one: dump_config prints either way, because it runs on the main
  // loop, not in this task.
  ESP_LOGI(TAG, "watchdog task running (tick_ms=%u)", (unsigned) millis());

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    if (!this->armed_)
      continue;  // no update() yet; do not judge a booting device

    const uint32_t starved_ms = millis() - this->last_beat_ms_;

    if (starved_ms <= this->timeout_ms_) {
      // Healthy. If we were stalled, update() is running again -- hand the
      // details to update() to persist and publish.
      if (this->in_stall_) {
        this->in_stall_ = false;
        this->last_stall_duration_ms_ = millis() - this->stall_started_ms_;
        this->stall_ended_pending_ = true;
      }
      continue;
    }

    // Enter the stall BEFORE any decision, so stall_started_ms_ is valid.
    if (!this->in_stall_) {
      this->in_stall_ = true;
      this->stall_started_ms_ = this->last_beat_ms_;
      this->stall_peak_divergence_ = 0;
      last_report_ms = 0;  // force one report immediately
    }

    // ---- TICK-ONLY decision path, evaluated BEFORE touching esp_timer ----
    // Ordering learned the hard way. The previous version read esp_timer first
    // and gated EVERY reboot on that reading. A live, correctly-configured
    // watchdog then sat through a 24.6 minute freeze without rebooting. If an
    // esp_timer read is itself unreliable while the timer is wedged (calling it
    // under GDB faulted hard enough to reset the chip), a design that must read
    // it before acting can be defeated by the very fault it guards against.
    // Everything here uses only millis() -- the FreeRTOS tick, which keeps
    // running through the fault (proven: tick 12,821,818 vs esp_timer
    // <11,409,498 in the JTAG capture).
    const uint32_t stall_ms = millis() - this->stall_started_ms_;
    if (this->reboot_ && this->reboot_after_ms_ != 0 && stall_ms >= this->reboot_after_ms_) {
      ESP_LOGE(TAG, "REBOOTING: scheduler stalled %u ms (tick clock) >= reboot_after %u ms",
               (unsigned) stall_ms, (unsigned) this->reboot_after_ms_);
      this->last_stall_duration_ms_ = stall_ms;
      this->nvs_bump_(KEY_STALLS, 0, true);
      this->nvs_bump_(KEY_REBOOTS, 0, false);
      this->nvs_add_stalled_(stall_ms);
      vTaskDelay(pdMS_TO_TICKS(750));
      esp_restart();
    }

    // ---- diagnostics, and the fast path when the clock is provably stalled --
    const int32_t div = this->clock_divergence_ms_();
    if (this->test_trip_)
      ESP_LOGW(TAG, "SELF-TEST: taking the real detection path now");
    // Measure the JUMP relative to the last healthy sample, not the absolute
    // divergence. SYSTIMER UNIT0 steps BACKWARD in discrete jumps and never
    // catches up, so the absolute deficit is cumulative and permanent -- using
    // it here would report "clock stalled" forever after the first stall and
    // silently defeat the OTA interlock below.
    const int32_t jump = this->have_healthy_divergence_ ? (div - this->healthy_divergence_ms_) : 0;
    const uint32_t abs_jump = (uint32_t) (jump < 0 ? -jump : jump);
    const uint32_t peak = (uint32_t) (this->stall_peak_divergence_ < 0 ? -this->stall_peak_divergence_
                                                                      : this->stall_peak_divergence_);
    if (abs_jump > peak)
      this->stall_peak_divergence_ = jump;  // record the STEP, not the running total
    const bool clock_stalled = abs_jump >= this->clock_stall_ms_;

    // At most one line a minute. An earlier version logged every second, which
    // would have turned a 25 minute stall into ~1500 identical lines.
    if (last_report_ms == 0 || (millis() - last_report_ms) >= 60000) {
      last_report_ms = millis();
      ESP_LOGE(TAG, "SCHEDULER STALLED %u ms (limit %u), esp_timer jumped %d ms "
                    "(cumulative divergence %d ms), clock_stalled=%s",
               (unsigned) stall_ms, (unsigned) this->timeout_ms_, (int) jump, (int) div,
               clock_stalled ? "YES" : "no");
    }

    if (!this->reboot_)
      continue;  // record only

    // Fast path: the clock has demonstrably stopped, so don't wait for
    // reboot_after. Requiring this is also the OTA interlock -- an OTA blocks
    // the loop but leaves esp_timer running, so divergence stays ~0.
    if (this->require_clock_stall_ ? clock_stalled : true) {
      ESP_LOGE(TAG, "REBOOTING: esp_timer jumped %d ms (cumulative %d) after %u ms stall",
               (int) jump, (int) div, (unsigned) stall_ms);
      this->last_stall_duration_ms_ = stall_ms;
      this->nvs_bump_(KEY_STALLS, jump, true);
      this->nvs_bump_(KEY_REBOOTS, jump, false);
      this->nvs_add_stalled_(stall_ms);
      vTaskDelay(pdMS_TO_TICKS(750));
      esp_restart();
    }
  }
}

void LoopWatchdog::dump_config() {
  ESP_LOGCONFIG(TAG, "Loop/Scheduler Watchdog:");
  ESP_LOGCONFIG(TAG, "  Stall threshold (update() starvation): %u ms", (unsigned) this->timeout_ms_);
  ESP_LOGCONFIG(TAG, "  Reboot on stall: %s", this->reboot_ ? "yes" : "no (record only)");
  if (this->reboot_) {
    if (this->require_clock_stall_)
      ESP_LOGCONFIG(TAG, "    requires clock divergence > %u ms (OTA-safe)",
                    (unsigned) this->clock_stall_ms_);
    else
      ESP_LOGCONFIG(TAG, "    clock confirmation DISABLED - reboots on any stall");
    if (this->reboot_after_ms_ != 0)
      ESP_LOGCONFIG(TAG, "    unconditional reboot after %u ms of stall",
                    (unsigned) this->reboot_after_ms_);
  }
  ESP_LOGCONFIG(TAG, "  Current clock divergence: %d ms", (int) this->clock_divergence_ms_());
}

}  // namespace loop_watchdog
}  // namespace esphome
