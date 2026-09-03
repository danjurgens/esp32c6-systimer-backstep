/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dan Jurgens
 * Part of https://github.com/danjurgens/esp32c6-systimer-backstep
 */
#include "clock_guard.h"
#include "esphome/core/log.h"
#include "esp_timer.h"
#include "esp_private/esp_timer_private.h"
#include "nvs_flash.h"
#include "nvs.h"

namespace esphome {
namespace clock_guard {

static const char *const TAG = "clock_guard";
static const char *const NVS_NS = "clkguard";

// millis() is tick-based (xTaskGetTickCount, SYSTIMER UNIT1).
// esp_timer_get_time() is UNIT0 -- the counter that steps backward.
// Positive result => esp_timer is BEHIND the tick, i.e. it lost time.
static void read_raw_counters_(uint64_t *u0, uint64_t *u1) {
  const uint32_t u0hi = *(volatile uint32_t *) 0x6000a040;
  const uint32_t u0lo = *(volatile uint32_t *) 0x6000a044;
  const uint32_t u1hi = *(volatile uint32_t *) 0x6000a048;
  const uint32_t u1lo = *(volatile uint32_t *) 0x6000a04c;
  *u0 = ((uint64_t) u0hi << 32) | u0lo;
  *u1 = ((uint64_t) u1hi << 32) | u1lo;
}

int32_t ClockGuard::divergence_ms_() {
  const uint32_t tick_ms = millis();
  const uint32_t esp_ms = (uint32_t) (esp_timer_get_time() / 1000);
  return (int32_t) (tick_ms - esp_ms);
}

void ClockGuard::nvs_restore_() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  uint32_t c = 0; int32_t l = 0; int64_t t = 0;
  nvs_get_u32(h, "count", &c);
  nvs_get_i32(h, "last", &l);
  nvs_get_i64(h, "total", &t);
  this->corrections_ = c;
  this->last_correction_ms_ = l;
  this->total_corrected_ms_ = t;
  nvs_close(h);
}

void ClockGuard::nvs_persist_() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGE(TAG, "NVS open failed; correction not persisted");
    return;
  }
  nvs_set_u32(h, "count", this->corrections_);
  nvs_set_i32(h, "last", this->last_correction_ms_);
  nvs_set_i64(h, "total", this->total_corrected_ms_);
  nvs_commit(h);
  nvs_close(h);
}

// The repair itself. Bracketed by esp_timer's own lock, matching how IDF does
// this in the light-sleep wake path (sleep_modes.c) -- that path calls
// esp_timer_private_lock() / _set() / _unlock() around the same counter write.
void ClockGuard::apply_correction_(int32_t excess_ms) {
  // Read the raw counters BEFORE repairing. Event #5 (2026-08-31 10:37) taught
  // this the hard way: the RAW dump sat after esp_timer_private_advance(), so
  // it recorded the already-repaired state (diff -3 ms) and the corrupted
  // value -- the entire point of the capture -- was destroyed by our own
  // repair one line earlier. The pre-repair UNIT1-UNIT0 is what the per-bit
  // analysis needs (UNIT0 == UNIT1 & ~mask, each bit verifiably set in UNIT1).
  uint64_t pre_u0 = 0, pre_u1 = 0;
  read_raw_counters_(&pre_u0, &pre_u1);
  ESP_LOGE(TAG, "PRE-REPAIR RAW UNIT0=%llu UNIT1=%llu  diff=%lld ticks",
           (unsigned long long) pre_u0, (unsigned long long) pre_u1,
           (long long) (pre_u1 - pre_u0));

  const int64_t us = (int64_t) excess_ms * 1000LL;
  esp_timer_private_lock();
  esp_timer_private_advance(us);
  esp_timer_private_unlock();

  this->corrections_++;
  this->last_correction_ms_ = excess_ms;
  this->total_corrected_ms_ += excess_ms;
  this->nvs_persist_();
  this->dirty_ = true;

  // NOTE: int32_t is 'long int' on riscv32-esp-elf, so %d is a -Wformat= error
  // here. Every other log line in this file already casts explicitly; this one
  // did not. Casting keeps the style uniform and avoids PRId32 clutter.
  ESP_LOGE(TAG, "CLOCK REPAIRED: esp_timer was %d ms (%.2f min) behind the tick; advanced it.",
           (int) excess_ms, (double) excess_ms / 60000.0);
  ESP_LOGE(TAG, "  correction #%u, cumulative %lld ms. loop_watchdog should NOT trip.",
           (unsigned) this->corrections_, (long long) this->total_corrected_ms_);

  // Log the RAW counters. clock_guard measures divergence against a maintained
  // baseline, which is not the same quantity as the jump itself -- baseline
  // drift and residual from a previous correction both fold in. The testbed's
  // 250 ms monitor poll measured a jump of exactly 2^37 ticks (one cleared
  // bit); the garage's baseline-relative numbers do NOT land on powers of two.
  // Without the raw values we cannot tell whether that is a measurement
  // artefact here or genuinely not a single-bit fault. UNIT1-UNIT0 is the
  // quantity to test: it should equal 2^N ticks.
  {
    const uint32_t u0hi = *(volatile uint32_t *) 0x6000a040;
    const uint32_t u0lo = *(volatile uint32_t *) 0x6000a044;
    const uint32_t u1hi = *(volatile uint32_t *) 0x6000a048;
    const uint32_t u1lo = *(volatile uint32_t *) 0x6000a04c;
    const uint64_t u0 = ((uint64_t) u0hi << 32) | u0lo;
    const uint64_t u1 = ((uint64_t) u1hi << 32) | u1lo;
    ESP_LOGE(TAG, "  POST-REPAIR RAW UNIT0=%llu UNIT1=%llu  diff=%lld ticks (should be ~0)",
             (unsigned long long) u0, (unsigned long long) u1,
             (long long) (u1 - u0));
  }
}

void ClockGuard::task_trampoline(void *arg) { static_cast<ClockGuard *>(arg)->task_loop(); }

void ClockGuard::task_loop() {
  // Establish the healthy baseline. The two counters share one 16 MHz source,
  // so their difference is a small constant; it moves only when tick
  // interrupts are missed (observed: ~200 ms after a long critical section).
  this->baseline_ms_ = this->divergence_ms_();
  this->have_baseline_ = true;
  ESP_LOGI(TAG, "guard running (interval %u ms, threshold %u ms, baseline %d ms)",
           (unsigned) this->check_interval_ms_, (unsigned) this->threshold_ms_,
           (int) this->baseline_ms_);

  for (;;) {
    const int32_t div = this->divergence_ms_();
    this->last_divergence_ms_ = div;
    const int32_t excess = div - this->baseline_ms_;

    if (excess > (int32_t) this->threshold_ms_) {
      // esp_timer lost time -- this is the fault.
      if (excess <= (int32_t) this->max_correction_ms_) {
        this->apply_correction_(excess);
        // Re-measure rather than assuming: the correction should have
        // returned divergence to the baseline.
        const int32_t after = this->divergence_ms_();
        ESP_LOGE(TAG, "  divergence after repair: %d ms (baseline %d)",
                 (int) after, (int) this->baseline_ms_);
      } else {
        // Beyond the sanity cap. Do NOT touch the clock; let loop_watchdog
        // reboot. A correction this large is more likely our bug than the
        // hardware's fault.
        this->refused_++;
        ESP_LOGE(TAG, "REFUSING correction of %d ms -- exceeds max_correction %u ms. "
                      "Leaving it to loop_watchdog.",
                 (int) excess, (unsigned) this->max_correction_ms_);
        // 2026-09-03 02:00 taught us this branch is where the WEIRD readings
        // land -- a +62 h divergence on a 15 h boot, impossible as a UNIT0
        // bit-clear (you cannot clear more than the counter holds), implying
        // esp_timer_get_time() returned NEGATIVE time. The refusal path
        // captured nothing, so the one reading that could classify it (raw
        // counters vs API clocks) was lost to the reboot. Dump it here,
        // rate-limited: first refusal and every 60th thereafter.
        if ((this->refused_ - 1) % 60 == 0) {
          uint64_t r0 = 0, r1 = 0;
          read_raw_counters_(&r0, &r1);
          const int64_t api_esp_us = esp_timer_get_time();
          const uint32_t api_ms = millis();
          ESP_LOGE(TAG, "REFUSAL RAW: UNIT0=%llu UNIT1=%llu diff=%lld ticks "
                        "(raw diff as ms: %lld)",
                   (unsigned long long) r0, (unsigned long long) r1,
                   (long long) (r1 - r0), (long long) ((r1 - r0) / 16000));
          ESP_LOGE(TAG, "REFUSAL API: esp_timer=%lld us, millis=%lu ms, "
                        "millis-esp/1000=%lld ms (raw-vs-API mismatch => "
                        "software timebase corrupt; match => counters really diverged)",
                   (long long) api_esp_us, (unsigned long) api_ms,
                   (long long) ((int64_t) api_ms - api_esp_us / 1000));
        }
      }
    } else if (excess < -1000) {
      // NEGATIVE excess means the TICK lost time (missed interrupts), not
      // esp_timer. Advancing esp_timer here would make things worse, so we
      // only re-baseline. Observed on the garage 08-26: a 199 ms tick slip
      // with no effect on the scheduler.
      ESP_LOGW(TAG, "tick slipped %d ms behind esp_timer; re-baselining (no repair)",
               (int) -excess);
      this->baseline_ms_ = div;
    } else {
      // Healthy: absorb slow drift so the baseline stays honest.
      this->baseline_ms_ = div;
    }

    vTaskDelay(pdMS_TO_TICKS(this->check_interval_ms_));
  }
}

void ClockGuard::setup() {
  this->nvs_restore_();
  xTaskCreate(&ClockGuard::task_trampoline, "clock_guard", 4096, this, 5, &this->task_handle_);
  if (this->task_handle_ == nullptr) {
    ESP_LOGE(TAG, "task create failed - clock NOT guarded (loop_watchdog still covers reboot)");
    this->mark_failed();
    return;
  }
  if (this->corrections_ > 0)
    ESP_LOGW(TAG, "restored from NVS: %u prior corrections, %lld ms cumulative",
             (unsigned) this->corrections_, (long long) this->total_corrected_ms_);

  // Publish once at startup so the entities show a real value (0, or the
  // NVS-restored count) instead of sitting at "unknown" until the first
  // correction ever happens.
  //
  // This is not cosmetic. An HA automation watching for an increment cannot
  // distinguish "unknown -> 1" from a restore, so without this the FIRST
  // correction -- the event we most want to be told about -- is either missed
  // or forces the automation to accept ambiguous transitions. Publishing a
  // baseline makes every subsequent change an unambiguous number -> number
  // increment.
  this->dirty_ = true;
}

// Publishing only. Never repairs here -- this runs in the scheduler, which is
// exactly what the fault freezes.
void ClockGuard::loop() {
  if (this->divergence_sensor_ != nullptr)
    this->divergence_sensor_->publish_state((float) this->last_divergence_ms_);
  if (!this->dirty_) return;
  this->dirty_ = false;
  // ORDER MATTERS. An HA automation naturally triggers on the COUNT (that is
  // the "a correction happened" signal) and then reads the other two to build
  // its message. If count published first, those reads race the sibling
  // publishes -- which is exactly what happened on 2026-08-29: a real 20.2 min
  // repair was announced as "0 min" because last_correction had not landed yet.
  // Publish the DETAIL first, the TRIGGER last.
  if (this->last_sensor_ != nullptr) this->last_sensor_->publish_state((float) this->last_correction_ms_);
  if (this->total_sensor_ != nullptr)
    this->total_sensor_->publish_state((float) (this->total_corrected_ms_ / 1000.0));
  if (this->count_sensor_ != nullptr) this->count_sensor_->publish_state((float) this->corrections_);
}

void ClockGuard::dump_config() {
  ESP_LOGCONFIG(TAG, "Clock Guard:");
  ESP_LOGCONFIG(TAG, "  check interval: %u ms", (unsigned) this->check_interval_ms_);
  ESP_LOGCONFIG(TAG, "  threshold: %u ms", (unsigned) this->threshold_ms_);
  ESP_LOGCONFIG(TAG, "  max correction: %u ms", (unsigned) this->max_correction_ms_);
  ESP_LOGCONFIG(TAG, "  prior corrections: %u", (unsigned) this->corrections_);
}

}  // namespace clock_guard
}  // namespace esphome
