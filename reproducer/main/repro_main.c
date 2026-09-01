/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Dan Jurgens
 * Part of https://github.com/danjurgens/esp32c6-systimer-backstep
 */
/*
 * Minimal reproducer: ESP32-C6 SYSTIMER UNIT0 (esp_timer) steps BACKWARD.
 *
 * Observed on Seeed XIAO ESP32-C6 rev0.2, ESP-IDF 5.5.4 and 5.5.5:
 * esp_timer's counter jumps backward by tens of minutes in a single step and
 * then resumes counting at the correct rate, while the FreeRTOS tick (a
 * different SYSTIMER unit) is unaffected. The deficit is permanent and
 * accumulates. Everything scheduled on esp_timer stalls until the counter
 * climbs back to previously-scheduled deadlines.
 *
 * This program does two things and nothing else:
 *   1. optionally hammers an I2C device (the suspected trigger),
 *   2. compares esp_timer against the FreeRTOS tick and against the raw
 *      SYSTIMER UNIT0/UNIT1 registers, and shouts if esp_timer moves backward.
 *
 * Build variants, all on the SAME board, to establish causation and
 * dose-response:
 *   idf.py build                          # I2C hammered at ~1 kHz (default)
 *   idf.py -DREPRO_I2C_PERIOD_MS=1000 build   # I2C at 1 Hz (field rate)
 *   idf.py -DREPRO_I2C=0 build            # I2C DISABLED (control)
 *
 * The field application transacts at ~1 Hz and jumps roughly once per day.
 * Hammering at ~1 kHz is ~1000x the transaction rate. If the fault is driven
 * by I2C activity, the jump rate should scale with it -- which is far stronger
 * evidence than presence/absence alone. The reported metric is therefore
 * JUMPS PER MILLION TRANSACTIONS, comparable across build variants.
 */
#include <stdio.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

/* ---- WiFi (REPRO_WIFI=1) --------------------------------------------------
 * THE VARIABLE UNDER TEST. Bare-IDF + I2C alone ran 20.43 h / 220,476
 * transactions with zero events, while ESPHome + WiFi + the SAME 3 tx/s I2C
 * pattern stalled at 9.2 h. The I2C pattern is now known to be faithful (the
 * driver does 1 data-ready poll + 1 block read + 1 interrupt clear per second),
 * so WiFi is the remaining difference.
 *
 * Modem sleep is enabled DELIBERATELY: ESPHome's default power_save_mode for
 * esp32 is "light" = WIFI_PS_MIN_MODEM, so both stalling nodes run with it,
 * while both quiet easystarts set power_save_mode: none. This build matches
 * the STALLING condition. See FINDINGS 34/35.
 */
#ifndef REPRO_WIFI
#define REPRO_WIFI 1
#endif
/* Modem sleep selects the CONSEQUENCE, not the fault: with WIFI_PS_MIN_MODEM
 * a backstep corrupts pp_timer_sleep_delay's math and the PHY path hangs ->
 * IWDT panic (demonstrates the worst case, but the reboot costs observation
 * time and can outrun the samplers). With WIFI_PS_NONE the same backstep just
 * freezes esp_timer consumers: the device stays up, every sampler keeps
 * running, and the recovery is observable. Default ON to match the original
 * report; set 0 for long observation runs. */
#ifndef REPRO_MODEM_SLEEP
#define REPRO_MODEM_SLEEP 1
#endif
#if REPRO_WIFI
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "repro_secrets.h"
static volatile uint32_t s_wifi_disconnects;
static volatile bool s_wifi_up;

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false; s_wifi_disconnects++;
        esp_wifi_connect();               /* keep the radio working, always */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_up = true;
    }
}

static void wifi_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL));
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", REPRO_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", REPRO_WIFI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(REPRO_MODEM_SLEEP ? WIFI_PS_MIN_MODEM : WIFI_PS_NONE));
}
#endif

#ifndef REPRO_I2C
#define REPRO_I2C 1
#endif

#if REPRO_I2C
#include "driver/i2c_master.h"
#define I2C_SDA_GPIO   22      /* XIAO C6 D4 */
#define I2C_SCL_GPIO   23      /* XIAO C6 D5 */
/* Bus speed is an EXPERIMENTAL VARIABLE. The field ran 100 kHz during the
 * frequent-failure period (4-5 stalls/day); it was dropped to 50 kHz on
 * 2026-08-21 as part of the same bundle that added external pull-ups and
 * switched drivers -- three changes at once, never separated.
 *
 * Speed matters most with weak pull-ups: at ~45k and ~100 pF the rise time is
 * ~4.5 us. A 50 kHz bit is 20 us (rise = 22% of it, workable); a 100 kHz bit
 * is 10 us (rise = ~45%, genuinely marginal). So 100 kHz + internal pull-ups
 * is the configuration that actually failed. */
#ifndef REPRO_I2C_HZ
#define REPRO_I2C_HZ   100000
#endif
#define I2C_HZ         REPRO_I2C_HZ
#define I2C_ADDR       0x29    /* VL53L1X; any responder works */
#define I2C_TIMEOUT_MS 50
/* Delay between transactions.
 *
 * Bus speed is kept at 50 kHz to MATCH THE FIELD CONFIGURATION, not because
 * the sensor requires it -- the VL53L1X supports 400 kbit/s Fast mode, so we
 * are 8x below its limit. The transaction used is a read of the model-ID
 * register (0x010F), which is static: register reads are not gated by the
 * ranging cycle, so there is no sensor-side rate ceiling.
 *
 * The limit is bus time. A 4-byte transfer at 50 kHz takes ~0.9 ms, so with a
 * 1 ms delay the achievable rate is ~525 transactions/s -- roughly 175x the
 * field application's ~3/s. If the fault is transaction-driven, that predicts
 * a backward step every ~8 minutes instead of once a day.
 *
 * NOTE: this test does not depend on the sensor cooperating. A NACK still
 * exercises the I2C peripheral and still counts as bus activity; errors are
 * tallied separately so a misbehaving device is visible rather than silent.
 *
 * If more rate is needed, raise scl_speed_hz rather than shrinking this delay
 * -- but that changes a second variable away from the field config. */
#ifndef REPRO_I2C_PERIOD_MS
#define REPRO_I2C_PERIOD_MS 1
#endif

/* Pull-up selection. THIS IS AN EXPERIMENTAL VARIABLE, not a detail.
 *
 * Field history: with the C6's internal pull-ups (~45 kOhm) the node stalled
 * 4-5x/day. After external 4.7k resistors were fitted on 2026-08-21 the rate
 * fell to ~1/day. That change landed together with a driver swap, so the two
 * were never separated -- this flag separates them.
 *
 *   REPRO_INTERNAL_PULLUP=0  external 4.7k fitted        (current hardware)
 *   REPRO_INTERNAL_PULLUP=1  internal ~45k, REMOVE the   (pre-2026-08-21 config)
 *                            external resistors first
 *
 * Weak pull-ups mean slow rise times and marginal signal integrity. If that is
 * what drives the SYSTIMER backward step, this build should reproduce far
 * faster than the current one, which has run 6x the field per-jump transaction
 * budget with zero events.
 *
 * Watch the error counter: with 45k at 50 kHz some NACK/timeout activity is
 * expected, and its rate is a usable proxy for how marginal the bus is. */
/* Field duty pattern: a short burst, then a long idle. See i2c_task(). */
#ifndef REPRO_BURST
#define REPRO_BURST 3
#endif
#ifndef REPRO_IDLE_MS
#define REPRO_IDLE_MS 1000
#endif
/* Idle is now sampled in chunks of this size so a backward step landing in the
 * idle window is caught promptly. FINDINGS 40 deliberately kept the idle as ONE
 * 1000 ms delay to avoid perturbing the idle behaviour under test -- that trade
 * cost us the phase on BOTH events (08-26 and 08-29), because the panic
 * followed the step faster than the I2C task's next check. The fidelity we
 * protected did not matter; the resolution we gave up is what we needed. */
#ifndef REPRO_IDLE_CHUNK_MS
#define REPRO_IDLE_CHUNK_MS 50
#endif

#ifndef REPRO_INTERNAL_PULLUP
#define REPRO_INTERNAL_PULLUP 0
#endif
static i2c_master_dev_handle_t s_dev;
static volatile uint64_t s_i2c_ok;    /* transactions completed */
static volatile uint64_t s_i2c_err;   /* transactions failed */
static volatile uint64_t s_i2c_bursts; /* idle->active cycles -- the suspected exposure */
#endif

static const char *TAG = "repro";

/* Persist findings across reboots. The run is intended to go days unattended,
 * and a RAM-only counter would be silently zeroed by any brownout or crash --
 * we would not know whether "0 backsteps" meant nothing happened or the
 * evidence was lost. */
#define NVS_NS "repro"
static uint32_t s_boot_count, s_total_backsteps;
static int64_t  s_worst_back_us_persist;

static void persist_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u32(h, "boots", &s_boot_count);
    nvs_get_u32(h, "backsteps", &s_total_backsteps);
    nvs_get_i64(h, "worst", &s_worst_back_us_persist);
    s_boot_count++;
    nvs_set_u32(h, "boots", s_boot_count);
    nvs_commit(h);
    nvs_close(h);
}

static void persist_phase(int kind, int idx, int64_t back_us, uint64_t tx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    s_total_backsteps++;
    nvs_set_u32(h, "backsteps", s_total_backsteps);
    nvs_set_i32(h, "ph_kind", kind);
    nvs_set_i32(h, "ph_idx", idx);
    nvs_set_i64(h, "ph_back", back_us);
    nvs_set_u64(h, "ph_tx", tx);
    nvs_commit(h);
    nvs_close(h);
}

static void persist_backstep(int64_t back_us, int64_t deficit_ms, uint64_t tx)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    s_total_backsteps++;
    nvs_set_u32(h, "backsteps", s_total_backsteps);
    if (back_us < s_worst_back_us_persist) {
        s_worst_back_us_persist = back_us;
        nvs_set_i64(h, "worst", back_us);
    }
    nvs_set_i64(h, "deficit", deficit_ms);
    nvs_set_u64(h, "tx", tx);
    nvs_commit(h);
    nvs_close(h);
}

/* SYSTIMER on ESP32-C6. These are the values last latched by the driver, which
 * is what esp_timer itself reads, so no update trigger is needed here.
 * Addresses hardcoded deliberately to keep this file header-independent. */
#define SYSTIMER_BASE        0x6000A000UL
#define SYSTIMER_CONF        (*(volatile uint32_t *)(SYSTIMER_BASE + 0x00))
#define SYSTIMER_U0_VAL_HI   (*(volatile uint32_t *)(SYSTIMER_BASE + 0x40))
#define SYSTIMER_U0_VAL_LO   (*(volatile uint32_t *)(SYSTIMER_BASE + 0x44))
#define SYSTIMER_U1_VAL_HI   (*(volatile uint32_t *)(SYSTIMER_BASE + 0x48))
#define SYSTIMER_U1_VAL_LO   (*(volatile uint32_t *)(SYSTIMER_BASE + 0x4C))

static inline uint64_t unit0_raw(void)
{
    uint32_t hi = SYSTIMER_U0_VAL_HI, lo = SYSTIMER_U0_VAL_LO;
    return ((uint64_t)hi << 32) | lo;
}
static inline uint64_t unit1_raw(void)
{
    uint32_t hi = SYSTIMER_U1_VAL_HI, lo = SYSTIMER_U1_VAL_LO;
    return ((uint64_t)hi << 32) | lo;
}

#if REPRO_I2C

/* ---- Phase-resolved backstep detection -------------------------------------
 * The monitor task polls every 250 ms, so it can only say "a jump happened
 * somewhere in the last quarter second". Checking esp_timer around each
 * individual transfer pins the jump to a specific TRANSACTION and PHASE.
 *
 * The open hypothesis (see i2c_task) is that the IDLE->ACTIVE transition is
 * the trigger rather than a transfer itself. These phases distinguish that:
 *
 *   XFER        inside i2c_master_transmit_receive  -> bus actively clocking
 *   INTER_XFER  between transfers inside a burst    -> short gap, periph awake
 *   IDLE        across the 1000 ms idle delay       -> periph gated down
 *   IDLE_WAKE   first read after idle, before the   -> THE TRANSITION
 *               next burst's first transfer
 *
 * DESIGN CONSTRAINT: the I2C duty pattern must not change. So the idle is
 * still ONE vTaskDelay(1000) -- not chunked -- because chunking would wake
 * the task 10x/s and alter exactly the idle behaviour under test. We trade
 * resolution *within* idle for fidelity, and still answer active-vs-idle.
 */
static const char *const PHASE_NAME[] = {"XFER", "INTER_XFER", "IDLE", "IDLE_WAKE"};
enum { PH_XFER = 0, PH_INTER = 1, PH_IDLE = 2, PH_WAKE = 3 };

static uint32_t s_phase_events;

static void report_phase_backstep(int64_t before_us, int64_t after_us,
                                  int kind, int idx)
{
    int64_t back = after_us - before_us;           /* negative */
    s_phase_events++;
    ESP_LOGE(TAG, "*** BACKSTEP DURING %s (idx %d) ***", PHASE_NAME[kind], idx);
    ESP_LOGE(TAG, "    esp_timer %lld -> %lld us  (moved %lld us = %.2f min)",
             (long long)before_us, (long long)after_us, (long long)back,
             (double)back / 60000000.0);
    ESP_LOGE(TAG, "    I2C SEQ: ok=%llu err=%llu burst=%llu  (this burst xfer %d/%d)",
             (unsigned long long)s_i2c_ok, (unsigned long long)s_i2c_err,
             (unsigned long long)s_i2c_bursts, idx + 1, REPRO_BURST);
    ESP_LOGE(TAG, "    tick=%" PRIu32 " ms  UNIT0=%llu UNIT1=%llu  CONF=0x%08" PRIx32,
             (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
             (unsigned long long)unit0_raw(), (unsigned long long)unit1_raw(),
             (uint32_t)SYSTIMER_CONF);
    /* Persist IMMEDIATELY: the 08-26 event panicked before the monitor task
     * could record anything, so nothing reached NVS. */
    persist_phase(kind, idx, back, s_i2c_ok);
}

static void i2c_task(void *arg)
{
    /* Replicate the FIELD DUTY PATTERN, not just the field throughput.
     *
     * The vl53l1x driver does a short burst -- data-ready poll, result read,
     * interrupt clear -- and then sleeps ~1 s. So every cycle the I2C
     * peripheral goes fully IDLE and is then woken again. Hammering at
     * 1000 tx/s removes exactly that: with ~0.55 ms between transfers the
     * peripheral never idles long enough to gate down, so there are almost no
     * idle->active transitions at all.
     *
     * If the trigger is a transition rather than a transfer, hammering
     * SUPPRESSES it. That would explain 15.8M transactions with zero events
     * against a field node that manages 4-5 events/day on ~260k transactions,
     * and it fits the pull-up and bus-speed sensitivity, since both shape the
     * edges at the start of a transfer coming up from idle.
     *
     * REPRO_BURST     transfers per burst (field does ~3)
     * REPRO_IDLE_MS   idle gap after each burst (field does ~1000)
     */
    uint8_t reg[2] = {0x01, 0x0F};   /* VL53L1X model-id register */
    uint8_t buf[2];
    int64_t t_prev = esp_timer_get_time();
    for (;;) {
        for (int i = 0; i < REPRO_BURST; i++) {
            /* gap between transfers (and, for i==0, the idle->active wake) */
            int64_t t_pre = esp_timer_get_time();
            if (t_pre < t_prev)
                report_phase_backstep(t_prev, t_pre, i == 0 ? PH_WAKE : PH_INTER, i);
            t_prev = t_pre;

            esp_err_t err = i2c_master_transmit_receive(s_dev, reg, sizeof(reg),
                                                        buf, sizeof(buf), I2C_TIMEOUT_MS);
            /* inside the transfer itself */
            int64_t t_post = esp_timer_get_time();
            if (t_post < t_prev)
                report_phase_backstep(t_prev, t_post, PH_XFER, i);
            t_prev = t_post;

            if (err == ESP_OK) {
                s_i2c_ok++;
            } else {
                if ((s_i2c_err++ % 100) == 0)
                    ESP_LOGW(TAG, "i2c err %s (total %llu)", esp_err_to_name(err),
                             (unsigned long long)s_i2c_err);
            }
        }
        s_i2c_bursts++;

        /* Idle, sampled every REPRO_IDLE_CHUNK_MS. The total idle time is
         * unchanged (chunks * chunk_ms == REPRO_IDLE_MS), so the DUTY CYCLE is
         * preserved; only the task wakes more often. idx records WHICH chunk,
         * so a hit also tells us how far into the idle the step landed. */
        int64_t t_idle = esp_timer_get_time();
        if (t_idle < t_prev)
            report_phase_backstep(t_prev, t_idle, PH_INTER, REPRO_BURST);
        for (int k = 0; k < (REPRO_IDLE_MS / REPRO_IDLE_CHUNK_MS); k++) {
            vTaskDelay(pdMS_TO_TICKS(REPRO_IDLE_CHUNK_MS));
            int64_t t_now = esp_timer_get_time();
            if (t_now < t_idle)
                report_phase_backstep(t_idle, t_now, PH_IDLE, k);
            t_idle = t_now;
        }
        t_prev = t_idle;
    }
}
#endif

static void monitor_task(void *arg)
{
    const uint32_t period_ms = 250;
    int64_t  prev_esp_us  = esp_timer_get_time();
    uint32_t prev_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    int64_t  worst_back_us = 0;
    uint32_t events = 0;

#if REPRO_I2C
    ESP_LOGI(TAG, "monitoring. SYSTIMER_CONF=0x%08" PRIx32 "  I2C ACTIVE, period %d ms, pullups=%s",
             (uint32_t)SYSTIMER_CONF, REPRO_I2C_PERIOD_MS,
             REPRO_INTERNAL_PULLUP ? "INTERNAL ~45k (weak, pre-08-21 config)" : "external 4.7k");
    ESP_LOGI(TAG, "bus speed %d Hz  (field ran 100000 during frequent failures)", I2C_HZ);
    ESP_LOGI(TAG, "duty pattern: %d transfers per burst, %d ms idle between "
                  "(field-like; hammering may SUPPRESS an idle->active trigger)",
             REPRO_BURST, REPRO_IDLE_MS);
#else
    ESP_LOGI(TAG, "monitoring. SYSTIMER_CONF=0x%08" PRIx32 "  I2C DISABLED (control build)",
             (uint32_t)SYSTIMER_CONF);
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(period_ms));

        int64_t  esp_us  = esp_timer_get_time();
        uint32_t tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        int64_t  d_esp_us  = esp_us - prev_esp_us;
        uint32_t d_tick_ms = tick_ms - prev_tick_ms;

        /* esp_timer must never move backward. */
        if (d_esp_us < 0) {
            events++;
            if (d_esp_us < worst_back_us) worst_back_us = d_esp_us;
            ESP_LOGE(TAG, "*** esp_timer WENT BACKWARD %lld us (%.1f min) ***",
                     (long long)d_esp_us, (double)d_esp_us / -60000000.0);
            ESP_LOGE(TAG, "    tick advanced %" PRIu32 " ms over the same interval", d_tick_ms);
            ESP_LOGE(TAG, "    UNIT0=%llu  UNIT1=%llu  (raw SYSTIMER ticks)",
                     (unsigned long long)unit0_raw(), (unsigned long long)unit1_raw());
            ESP_LOGE(TAG, "    SYSTIMER_CONF=0x%08" PRIx32 "  (compare against boot value)",
                     (uint32_t)SYSTIMER_CONF);
            ESP_LOGE(TAG, "    event #%" PRIu32 ", worst so far %lld us", events, (long long)worst_back_us);
            persist_backstep(d_esp_us, (int64_t)tick_ms - esp_us / 1000,
#if REPRO_I2C
                             s_i2c_ok);
#else
                             0);
#endif
            ESP_LOGE(TAG, "    PERSISTED: lifetime backsteps=%" PRIu32 " across %" PRIu32 " boots",
                     s_total_backsteps, s_boot_count);
#if REPRO_I2C
            ESP_LOGE(TAG, "    i2c transactions so far: %llu ok, %llu err",
                     (unsigned long long)s_i2c_ok, (unsigned long long)s_i2c_err);
            if (s_i2c_ok)
                ESP_LOGE(TAG, "    rate: %.3f jumps per MILLION transactions",
                         (double)events * 1e6 / (double)s_i2c_ok);
#endif
        }

        prev_esp_us  = esp_us;
        prev_tick_ms = tick_ms;

        /* Heartbeat once a minute: esp_timer vs tick, and the running deficit. */
        static uint32_t n;
        if (++n % (60000 / period_ms) == 0) {
            int64_t esp_ms = esp_us / 1000;
#if REPRO_I2C
            ESP_LOGI(TAG, "esp_timer %lld ms | tick %" PRIu32 " ms | deficit %lld ms | backsteps %"
                          PRIu32 " | i2c ok=%llu err=%llu (%.0f tx/s)",
                     (long long)esp_ms, tick_ms, (long long)((int64_t)tick_ms - esp_ms), events,
                     (unsigned long long)s_i2c_ok, (unsigned long long)s_i2c_err,
                     tick_ms ? (double)s_i2c_ok * 1000.0 / (double)tick_ms : 0.0);
            ESP_LOGI(TAG, "  bursts (idle->active cycles) %llu", (unsigned long long)s_i2c_bursts);
#if REPRO_WIFI
            /* WiFi is THE VARIABLE UNDER TEST. Without this line a null result
             * after days of running would be uninterpretable -- we could not
             * tell "WiFi up, no stall" from "WiFi silently dropped". */
            ESP_LOGI(TAG, "  wifi %s | disconnects %" PRIu32 " | ps=%s",
                     s_wifi_up ? "UP" : "DOWN", s_wifi_disconnects,
                     REPRO_MODEM_SLEEP ? "MIN_MODEM" : "NONE");
#endif
#else
            ESP_LOGI(TAG, "esp_timer %lld ms | tick %" PRIu32 " ms | deficit %lld ms | backsteps %"
                          PRIu32 " | I2C DISABLED (control)",
                     (long long)esp_ms, tick_ms, (long long)((int64_t)tick_ms - esp_ms), events);
#endif
        }
    }
}

void app_main(void)
{
    esp_err_t nv = nvs_flash_init();
    if (nv == ESP_ERR_NVS_NO_FREE_PAGES || nv == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    persist_load();
#if REPRO_WIFI
    wifi_start();
#endif
    ESP_LOGW(TAG, "boot #%" PRIu32 " | lifetime backsteps %" PRIu32 " | worst %lld us",
             s_boot_count, s_total_backsteps, (long long)s_worst_back_us_persist);
    {   /* Report the phase of the last recorded backstep. Survives the
         * panic-reboot that cost us the 08-26 event's context entirely. */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            int32_t k = -1, ix = -1; int64_t bk = 0; uint64_t tx = 0;
            nvs_get_i32(h, "ph_kind", &k); nvs_get_i32(h, "ph_idx", &ix);
            nvs_get_i64(h, "ph_back", &bk); nvs_get_u64(h, "ph_tx", &tx);
            if (k >= 0 && k <= 3)
                ESP_LOGW(TAG, "LAST BACKSTEP PHASE: %s idx=%" PRId32
                              " back=%lld us at tx #%llu",
                         PHASE_NAME[k], ix, (long long)bk,
                         (unsigned long long)tx);
            else
                ESP_LOGW(TAG, "no phase-resolved backstep recorded yet");
            nvs_close(h);
        }
    }
#if REPRO_WIFI
    ESP_LOGW(TAG, "WiFi ENABLED, ps=%s", REPRO_MODEM_SLEEP ? "MIN_MODEM (panic path armed)" : "NONE (observation mode)");
#else
    ESP_LOGW(TAG, "WiFi DISABLED (control)");
#endif
#if REPRO_I2C
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = REPRO_INTERNAL_PULLUP ? true : false,
    };
    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev));

    xTaskCreate(i2c_task, "i2c", 4096, NULL, 5, NULL);
#endif
    xTaskCreate(monitor_task, "monitor", 4096, NULL, 4, NULL);
}
