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
#include "esp_private/esp_timer_private.h"
#include "esp_system.h"

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
/* ---- Time-warp (REPRO_TIMEWARP=1) -----------------------------------------
 * The fault clears already-SET counter bits, so bits are only testable once
 * uptime has set them: bit 40 needs 19 h, bit 45 needs 24 DAYS, bit 50 needs
 * 2.2 years. Rather than wait, preset the counter into the future with
 * esp_timer_private_set() -- IDF's own API, the one the light-sleep resync
 * uses -- so the whole upper range is clearable from minute one.
 *
 * Preset = 2^51 - 2^41 ticks (bits 41..50 set, bit 51 clear):
 *   - wrap horizon 4.5 years (bit 51 guards the top)
 *   - bits 26..40 fill on their natural schedule, as before
 *   - natural carry rolls 41..50 over after 2^41 ticks = 38.1 h, so the
 *     high-bit observation window is ~38 h per boot; re-preset by rebooting.
 * Detection is delta-based, so a cleared high bit (a days-to-weeks backward
 * jump) is caught exactly like any other. NOTE: with the counter warped,
 * UNIT1-UNIT0 is hugely negative; external tooling comparing the two must
 * subtract the preset. */
#ifndef REPRO_TIMEWARP
#define REPRO_TIMEWARP 0
#endif
/* With TIMEWARP, the preset's high bits erode two ways: events clear them,
 * and natural carry rolls 41..50 over at 2^41 ticks = 38.1 h. Since the
 * preset reapplies in app_main, a REBOOT restores full witness density.
 * Auto-reboot on the TICK clock (immune to the fault) before rollover. */
#ifndef REPRO_REWARP_HOURS
#define REPRO_REWARP_HOURS 34
#endif
/* One-shot ranging sanity check at boot: initialise the VL53L1X for ranging,
 * read a single distance, log it, then hand back to model-id hammering. Init
 * sequence lifted verbatim from the field vl53l1x_tof component. Default on;
 * costs one measurement (~50 ms) at startup and nothing thereafter. */
#ifndef REPRO_RANGE_CHECK
#define REPRO_RANGE_CHECK 1
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
        if (s_wifi_disconnects <= 12) {
            const wifi_event_sta_disconnected_t *d = data;
            ESP_LOGW("repro", "wifi disconnect #%" PRIu32 ": reason %u rssi %d",
                     s_wifi_disconnects, d->reason, d->rssi);
        }
        esp_wifi_connect();               /* keep the radio working, always */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_wifi_up = true;
    }
}

/* Seeed XIAO ESP32-C6: the antenna is behind an RF switch. GPIO3 LOW powers the
 * switch, GPIO14 LOW selects the on-board ceramic antenna (HIGH = U.FL).
 * Seeed's Arduino core does this in initVariant(); bare IDF does NOT, leaving
 * the switch at whatever the floating pins read -- observed 2026-09-23 as ~25
 * failed associations/min on a XIAO where a DevKitC-1 on the same bench, same
 * firmware and credentials, joined instantly. On other boards GPIO3/14 are
 * ordinary pins; set REPRO_XIAO_RF=0 if anything is wired to them. */
#ifndef REPRO_XIAO_RF
#define REPRO_XIAO_RF 1
#endif
#if REPRO_XIAO_RF
#include "driver/gpio.h"
static void xiao_rf_switch_onboard(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << 3) | (1ULL << 14),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(3, 0);      /* RF switch power on */
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(14, 0);     /* on-board antenna */
}
#endif

static void wifi_start(void)
{
#if REPRO_XIAO_RF
    xiao_rf_switch_onboard();
#endif
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
#ifndef REPRO_SDA_GPIO
#define REPRO_SDA_GPIO 22      /* XIAO C6 D4 */
#endif
#ifndef REPRO_SCL_GPIO
#define REPRO_SCL_GPIO 23      /* XIAO C6 D5 */
#endif
#define I2C_SDA_GPIO   REPRO_SDA_GPIO
#define I2C_SCL_GPIO   REPRO_SCL_GPIO
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
static i2c_master_bus_handle_t s_bus;
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
/* Why the previous boot ended and how long it had run (tick time, saved every
 * 10 min). An unexplained reboot on 2026-11-04 couldn't be classified because
 * the reason was only ever printed in a banner nobody was reading. */
#define UPTIME_SAVE_MIN 10
static uint32_t s_prev_up_min;

static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "EFUSE";
    case ESP_RST_PWR_GLITCH:return "PWR_GLITCH";
    case ESP_RST_CPU_LOCKUP:return "CPU_LOCKUP";
    default:                return "UNKNOWN";
    }
}

static void persist_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_get_u32(h, "boots", &s_boot_count);
    nvs_get_u32(h, "backsteps", &s_total_backsteps);
    nvs_get_i64(h, "worst", &s_worst_back_us_persist);
    nvs_get_u32(h, "up_min", &s_prev_up_min);
    s_boot_count++;
    nvs_set_u32(h, "boots", s_boot_count);
    nvs_set_u32(h, "up_min", 0);
    /* reset-reason history: last 8, newest first, "boot#:REASON:upmin" */
    char hist[200] = "", line[240];
    size_t hl = sizeof(hist);
    nvs_get_str(h, "rst_hist", hist, &hl);
    snprintf(line, sizeof(line), "%" PRIu32 ":%s:%" PRIu32 " %s", s_boot_count,
             reset_reason_name(esp_reset_reason()), s_prev_up_min, hist);
    int sp = 0;
    for (char *p = line; *p; p++)
        if (*p == ' ' && ++sp == 8) { *p = 0; break; }
    nvs_set_str(h, "rst_hist", line);
    nvs_commit(h);
    nvs_close(h);
}

static void persist_uptime(uint32_t up_min)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, "up_min", up_min);
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

/* Load registers: the value a unit takes when its LOAD bit is triggered. Only
 * the low 20 bits of HI exist (52-bit counter). */
#define SYSTIMER_U0_LOAD_HI  (*(volatile uint32_t *)(SYSTIMER_BASE + 0x0C))
#define SYSTIMER_U0_LOAD_LO  (*(volatile uint32_t *)(SYSTIMER_BASE + 0x10))
#define SYSTIMER_U1_LOAD_HI  (*(volatile uint32_t *)(SYSTIMER_BASE + 0x14))
#define SYSTIMER_U1_LOAD_LO  (*(volatile uint32_t *)(SYSTIMER_BASE + 0x18))

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
static inline uint64_t load0_raw(void)
{
    return ((uint64_t)(SYSTIMER_U0_LOAD_HI & 0xFFFFF) << 32) | SYSTIMER_U0_LOAD_LO;
}
static inline uint64_t load1_raw(void)
{
    return ((uint64_t)(SYSTIMER_U1_LOAD_HI & 0xFFFFF) << 32) | SYSTIMER_U1_LOAD_LO;
}

/* ---- Load-register canary (REPRO_LOAD_CANARY=1) ----------------------------
 * Requested by Espressif on #19036 (2026-11-04). Every raw capture so far shows
 * one whole bit group of UNIT0 zeroed, [26..35] or [36..51], and the UNIT0
 * load registers read 0. So the group was either reset, or spuriously LOADED
 * from those registers. Writing a recognisable pattern into the load registers
 * at boot tells the two apart: after an event the cleared group holds the
 * canary's bits (spurious load) or zeros (reset). The LOAD trigger is never
 * written, so the counter itself is not touched. Equivalent to
 * systimer_ll_set_counter_value(&SYSTIMER, 0, REPRO_CANARY). */
#ifndef REPRO_LOAD_CANARY
#define REPRO_LOAD_CANARY 1
#endif
#define REPRO_CANARY ((0x5A5A5ULL << 32) | 0xA5A5A5A5ULL)
#define GRP_LO(v) ((uint32_t)(((v) >> 26) & 0x3FF))     /* bits 26..35 */
#define GRP_HI(v) ((uint32_t)(((v) >> 36) & 0xFFFF))    /* bits 36..51 */

static void load_canary_arm(void)
{
#if REPRO_LOAD_CANARY
    SYSTIMER_U0_LOAD_HI = (uint32_t)(REPRO_CANARY >> 32);
    SYSTIMER_U0_LOAD_LO = (uint32_t)REPRO_CANARY;
    /* No readback: the registers are only read at event time (report_raw). */
    ESP_LOGW(TAG, "LOAD CANARY: UNIT0 load regs written 0x%013llx; "
                      "group bits [26..35]=0x%03" PRIx32 " [36..51]=0x%04" PRIx32,
             (unsigned long long)REPRO_CANARY,
             GRP_LO(REPRO_CANARY), GRP_HI(REPRO_CANARY));
#endif
}

/* ---- GPTimer witness (REPRO_WITNESS=1) --------------------------------------
 * A third counter, in a different peripheral (timer group 0), clocked from the
 * same 40 MHz XTAL as SYSTIMER so the two cannot drift apart. It tells which
 * SYSTIMER unit moved, catches UNIT1 losses (the FreeRTOS tick just lags, it
 * never steps back), and catches UNIT0 losses shorter than the 250 ms poll,
 * which the backward check cannot see. At event time it also predicts what
 * each unit SHOULD hold, so the XOR against the actual value shows which bits
 * changed. Reads only TIMG registers; SYSTIMER is touched once at boot for the
 * baseline and otherwise only at event time, as before. */
#ifndef REPRO_WITNESS
#define REPRO_WITNESS 1
#endif
#define WITNESS_MS 15   /* above tick quantisation + scheduling jitter */
#if REPRO_WITNESS
#include "driver/gptimer.h"
static gptimer_handle_t s_gpt;
static uint64_t s_base_gpt_us, s_base_u0, s_base_u1;
static int64_t  s_base_esp_us;
static uint32_t s_base_tick_ms;
static uint32_t s_witness_events;

static inline uint64_t gpt_us(void)
{
    uint64_t v = 0;
    gptimer_get_raw_count(s_gpt, &v);
    return v;
}

static void witness_start(void)
{
    gptimer_config_t cfg = {
        .clk_src = GPTIMER_CLK_SRC_XTAL,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&cfg, &s_gpt));
    ESP_ERROR_CHECK(gptimer_enable(s_gpt));
    ESP_ERROR_CHECK(gptimer_start(s_gpt));
    /* Right after a tick the latched UNIT1 value is fresh; esp_timer_get_time()
     * latches UNIT0. Baseline error is then a few us, well below a bit-10 step. */
    vTaskDelay(1);
    s_base_esp_us  = esp_timer_get_time();
    s_base_u0      = unit0_raw();
    s_base_u1      = unit1_raw();
    s_base_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_base_gpt_us  = gpt_us();
    ESP_LOGW(TAG, "WITNESS: GPTimer (TIMG, XTAL, 1 MHz) baseline UNIT0=0x%013llx UNIT1=0x%013llx; "
                  "alerts at >= %d ms disagreement",
             (unsigned long long)s_base_u0, (unsigned long long)s_base_u1, WITNESS_MS);
}
#endif

/* Raw capture for every backstep report: both counters, both load registers,
 * and each counter's two bit groups next to the canary's, so a reader can see
 * at a glance whether a cleared group holds zeros or the canary. Persisted to
 * NVS (replayed on the next boot banner) because with modem sleep on the IWDT
 * panic can follow the step before the console drains. Several reporters see
 * the same event; only the first within 1 s of tick time is persisted. */
static uint32_t s_raw_saved_tick;
static bool s_raw_saved;

static void report_raw(void)
{
    uint64_t u0 = unit0_raw(), u1 = unit1_raw(), l0 = load0_raw(), l1 = load1_raw();
    uint32_t tick = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    ESP_LOGE(TAG, "    RAW UNIT0=0x%013llx UNIT1=0x%013llx LOAD0=0x%013llx LOAD1=0x%013llx",
             (unsigned long long)u0, (unsigned long long)u1,
             (unsigned long long)l0, (unsigned long long)l1);
    ESP_LOGE(TAG, "    groups [26..35] UNIT0=0x%03" PRIx32 " UNIT1=0x%03" PRIx32 " canary=0x%03" PRIx32
                  " | [36..51] UNIT0=0x%04" PRIx32 " UNIT1=0x%04" PRIx32 " canary=0x%04" PRIx32 "%s",
             GRP_LO(u0), GRP_LO(u1), GRP_LO(REPRO_CANARY),
             GRP_HI(u0), GRP_HI(u1), GRP_HI(REPRO_CANARY),
             (REPRO_LOAD_CANARY && l0 != REPRO_CANARY) ? "  ** LOAD0 != canary: overwritten **" : "");
#if REPRO_WITNESS
    uint64_t g = gpt_us();
    uint64_t el = (g - s_base_gpt_us) * 16ULL;   /* 16 SYSTIMER ticks per us */
    uint64_t e0 = s_base_u0 + el, e1 = s_base_u1 + el;
    /* UNIT1's latched value can be up to 1 ms (16,000 ticks, ~bit 14) stale. */
    ESP_LOGE(TAG, "    WITNESS GPTimer %llu us | expect UNIT0=0x%013llx (actual %+.3f ms, xor 0x%013llx)"
                  " | expect UNIT1=0x%013llx (actual %+.3f ms, xor 0x%013llx)",
             (unsigned long long)g,
             (unsigned long long)e0, (double)((int64_t)(u0 - e0)) / 16000.0, (unsigned long long)(u0 ^ e0),
             (unsigned long long)e1, (double)((int64_t)(u1 - e1)) / 16000.0, (unsigned long long)(u1 ^ e1));
#endif
    if (s_raw_saved && tick - s_raw_saved_tick < 1000) return;
    s_raw_saved = true;
    s_raw_saved_tick = tick;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u64(h, "raw_u0", u0);
    nvs_set_u64(h, "raw_u1", u1);
    nvs_set_u64(h, "raw_l0", l0);
    nvs_set_u64(h, "raw_l1", l1);
    nvs_set_u32(h, "raw_tick", tick);
    nvs_set_u32(h, "raw_boot", s_boot_count);
#if REPRO_WITNESS
    nvs_set_u64(h, "raw_gpt", g);
#endif
    nvs_commit(h);
    nvs_close(h);
}

/* Phase names for phase-resolved backstep reports. Shared by every peripheral
 * mode (and read back from NVS in app_main), so they live outside any mode
 * guard. PING is the ultrasonic trigger->echo window. */
static const char *const PHASE_NAME[] = {"XFER", "INTER_XFER", "IDLE", "IDLE_WAKE", "PING"};
enum { PH_XFER = 0, PH_INTER = 1, PH_IDLE = 2, PH_WAKE = 3, PH_PING = 4 };
#define PH_COUNT 5

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
    report_raw();
    /* Persist IMMEDIATELY: the 08-26 event panicked before the monitor task
     * could record anything, so nothing reached NVS. */
    persist_phase(kind, idx, back, s_i2c_ok);
}


#if REPRO_RANGE_CHECK
/* 16-bit-addressed I2C helpers over the existing device handle, and a one-shot
 * VL53L1X range read. Mirrors my_components/vl53l1x_tof exactly. */
static esp_err_t vl_w8(uint16_t reg, uint8_t v) {
    uint8_t b[3] = {(uint8_t)(reg>>8),(uint8_t)reg,v};
    return i2c_master_transmit(s_dev, b, 3, I2C_TIMEOUT_MS);
}
static esp_err_t vl_w16(uint16_t reg, uint16_t v) {
    uint8_t b[4] = {(uint8_t)(reg>>8),(uint8_t)reg,(uint8_t)(v>>8),(uint8_t)v};
    return i2c_master_transmit(s_dev, b, 4, I2C_TIMEOUT_MS);
}
static esp_err_t vl_w32(uint16_t reg, uint32_t v) {
    uint8_t b[6] = {(uint8_t)(reg>>8),(uint8_t)reg,
                    (uint8_t)(v>>24),(uint8_t)(v>>16),(uint8_t)(v>>8),(uint8_t)v};
    return i2c_master_transmit(s_dev, b, 6, I2C_TIMEOUT_MS);
}
static esp_err_t vl_r8(uint16_t reg, uint8_t *v) {
    uint8_t a[2] = {(uint8_t)(reg>>8),(uint8_t)reg};
    return i2c_master_transmit_receive(s_dev, a, 2, v, 1, I2C_TIMEOUT_MS);
}
static esp_err_t vl_r16(uint16_t reg, uint16_t *v) {
    uint8_t a[2] = {(uint8_t)(reg>>8),(uint8_t)reg}, r[2];
    esp_err_t e = i2c_master_transmit_receive(s_dev, a, 2, r, 2, I2C_TIMEOUT_MS);
    if (e==ESP_OK) *v = ((uint16_t)r[0]<<8)|r[1];
    return e;
}
static esp_err_t vl_rblock(uint16_t reg, uint8_t *buf, size_t len) {
    uint8_t a[2] = {(uint8_t)(reg>>8),(uint8_t)reg};
    return i2c_master_transmit_receive(s_dev, a, 2, buf, len, I2C_TIMEOUT_MS);
}

static void range_sanity_check(void) {
#define RK(x) do{ if((x)!=ESP_OK){ ESP_LOGW(TAG,"RANGE: I2C fail @%d",__LINE__); return; } }while(0)
    /* Release SOFT_RESET (0x0000) first. A sensor left in soft reset still
     * ACKs but reads 0x00 everywhere, and only a power cycle clears it
     * (XSHUT is tied high), so every MCU reset after it got stuck there
     * looked like a dead sensor. */
    RK(vl_w8(0x0000,0x01)); vTaskDelay(pdMS_TO_TICKS(2));
    uint16_t mid=0; RK(vl_r16(0x010F,&mid));
    if (mid != 0xEACC) {
        /* Not a VL53L1X. A VL53L0X also sits at 0x29 but uses 8-bit register
         * addresses; its ID register 0xC0 reads 0xEE. */
        uint8_t r = 0xC0, id8 = 0;
        esp_err_t e = i2c_master_transmit_receive(s_dev, &r, 1, &id8, 1, I2C_TIMEOUT_MS);
        ESP_LOGW(TAG,"RANGE: unexpected model id 0x%04X; 8-bit reg 0xC0 = 0x%02X (%s)%s (skip)",
                 mid, id8, esp_err_to_name(e), id8 == 0xEE ? " -> looks like a VL53L0X" : "");
        /* Nothing lives at 0x30. If it ACKs too, SDA is held low (every bit
         * reads as ACK / 0), i.e. a wiring or power fault, not a sensor ID. */
        ESP_LOGW(TAG,"RANGE: probe of empty address 0x30: %s",
                 esp_err_to_name(i2c_master_probe(s_bus, 0x30, I2C_TIMEOUT_MS)));
        char found[200] = ""; size_t o = 0;
        for (uint16_t a = 0x08; a < 0x78; a++)
            if (i2c_master_probe(s_bus, a, I2C_TIMEOUT_MS) == ESP_OK)
                o += snprintf(found + o, sizeof(found) - o, " 0x%02X", a);
        ESP_LOGW(TAG,"RANGE: bus scan:%s", o ? found : " (none)");
        /* raw ID-ish registers: 16-bit addressed (VL53L1X/VL6180X), 8-bit addressed */
        uint8_t b[4] = {0};
        vl_rblock(0x0000, b, 4);
        uint8_t id16[2] = {0}; vl_rblock(0x010F, id16, 2);
        uint8_t r8[3] = {0x00, 0x12, 0x92}, v8[3] = {0};
        for (int i = 0; i < 3; i++)
            i2c_master_transmit_receive(s_dev, &r8[i], 1, &v8[i], 1, I2C_TIMEOUT_MS);
        ESP_LOGW(TAG,"RANGE: reg16[0x0000..3]=%02X %02X %02X %02X reg16[0x010F]=%02X%02X "
                     "reg8[0x00]=%02X reg8[0x12]=%02X reg8[0x92]=%02X",
                 b[0], b[1], b[2], b[3], id16[0], id16[1], v8[0], v8[1], v8[2]);
        return;
    }
    RK(vl_w8(0x0000,0x00)); esp_rom_delay_us(100);
    RK(vl_w8(0x0000,0x01)); vTaskDelay(pdMS_TO_TICKS(1));
    uint32_t t0=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS); uint8_t st=0;
    do { if(vl_r8(0x00E5,&st)!=ESP_OK){ESP_LOGW(TAG,"RANGE: boot poll fail");return;}
         if(((uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS))-t0>100){ESP_LOGW(TAG,"RANGE: boot timeout");return;}
    } while((st&0x01)==0);
    uint8_t v2e=0; RK(vl_r8(0x002E,&v2e)); RK(vl_w8(0x002E,v2e|0x01));
    uint16_t fast_osc=0, osc_cal=0; RK(vl_r16(0x0006,&fast_osc)); RK(vl_r16(0x00DE,&osc_cal));
    if(!osc_cal) osc_cal=1;
    RK(vl_w16(0x0024,0x0A00)); RK(vl_w8(0x0031,0x02));
    RK(vl_w8(0x0036,0x08)); RK(vl_w8(0x0037,0x10)); RK(vl_w8(0x0039,0x01));
    RK(vl_w8(0x003E,0xFF)); RK(vl_w8(0x003F,0x00)); RK(vl_w8(0x0040,0x02));
    RK(vl_w16(0x0050,0x0000)); RK(vl_w16(0x0052,0x0000)); RK(vl_w8(0x0057,0x38));
    RK(vl_w16(0x0064,360)); RK(vl_w16(0x0066,192)); RK(vl_w8(0x0071,0x01)); RK(vl_w8(0x007C,0x01));
    RK(vl_w8(0x007E,0x02)); RK(vl_w8(0x0082,0x00)); RK(vl_w8(0x0077,0x01)); RK(vl_w8(0x0081,0x8B));
    RK(vl_w16(0x0054,200<<8)); RK(vl_w8(0x004F,0x02)); RK(vl_w8(0x0060,0x0F)); RK(vl_w8(0x0063,0x0D));
    RK(vl_w8(0x0069,0xB8)); RK(vl_w8(0x0078,0x0F)); RK(vl_w8(0x0079,0x0D)); RK(vl_w8(0x007A,14)); RK(vl_w8(0x007B,14));
    uint32_t pll = ((uint32_t)0x01<<30)/fast_osc;
    uint8_t va=(0x0F+1)<<1; uint32_t ma=((uint32_t)2304*pll)>>6; ma*=va; ma>>=6;
    uint8_t vb=(0x0D+1)<<1; uint32_t mb=((uint32_t)2304*pll)>>6; mb*=vb; mb>>=6;
    #define MCA(us) ((((uint32_t)(us)<<12)+(ma>>1))/ma)
    #define MCB(us) ((((uint32_t)(us)<<12)+(mb>>1))/mb)
    #define ENC(mclks) ({ uint32_t _m=(mclks); uint16_t _r=0; if(_m>0){uint32_t _ls=_m-1; uint16_t _ms=0; \
                          while((_ls&0xFFFFFF00)>0){_ls>>=1;_ms++;} _r=(_ms<<8)|(_ls&0xFF);} _r; })
    uint32_t rt=22736; uint32_t pc=MCA(1000); if(pc>0xFF)pc=0xFF;
    RK(vl_w8(0x004B,(uint8_t)pc));
    RK(vl_w16(0x005E,ENC(MCA(rt)))); RK(vl_w16(0x0061,ENC(MCB(rt))));
    RK(vl_w16(0x005A,ENC(MCA(1))));  RK(vl_w16(0x005C,ENC(MCB(1))));
    uint16_t osc22=0; RK(vl_r16(0x0022,&osc22)); RK(vl_w16(0x001E,osc22*4));
    RK(vl_w32(0x006C,100*osc_cal));
    RK(vl_w8(0x0086,0x01)); RK(vl_w8(0x0087,0x40));
    /* wait for one result, bounded ~600 ms */
    uint32_t w0=(uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS); uint8_t gs=0;
    for(;;){ if(vl_r8(0x0031,&gs)!=ESP_OK){ESP_LOGW(TAG,"RANGE: status fail");return;}
             if((gs&0x01)==0) break;
             if(((uint32_t)(xTaskGetTickCount()*portTICK_PERIOD_MS))-w0>600){ESP_LOGW(TAG,"RANGE: no result in 600ms");return;}
             vTaskDelay(pdMS_TO_TICKS(20)); }
    uint8_t d[17]; RK(vl_rblock(0x0089,d,17));
    uint8_t rstatus=d[0]; uint16_t raw=((uint16_t)d[13]<<8)|d[14];
    uint32_t mm=((uint32_t)raw*2011+0x0400)/0x0800;
    ESP_LOGW(TAG,"RANGE SANITY: %lu mm (%.2f m), range_status=%u  -- sensor optically OK",
             (unsigned long)mm, mm/1000.0f, rstatus);
    /* Leave the sensor stopped (SYSTEM__MODE_START = 0) but out of reset; the
     * model-id hammer does not need ranging active. This used to write
     * SOFT_RESET = 0, which held the sensor in reset until a power cycle. */
    vl_w8(0x0086,0x01); vl_w8(0x0087,0x00);
#undef RK
}
#endif

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
#if REPRO_RANGE_CHECK
    range_sanity_check();
#endif
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

/* ---- Ultrasonic load (REPRO_ULTRASONIC=1) ------------------------------------
 * Replaces the I2C load with an RCWL-1601 / HC-SR04-style trig/echo sensor, to
 * test whether the backstep needs I2C at all or any radio + peripheral activity
 * will do (a third-party report reproduced it with RMT + ADC and no I2C).
 * Build with I2C off:  idf.py -DREPRO_I2C=0 -DREPRO_ULTRASONIC=1 build
 *
 * The echo pulse is measured by the RMT peripheral in hardware (1 us
 * resolution), NOT by timestamping edges with esp_timer, so a backstep cannot
 * corrupt the distance. The trigger uses esp_rom_delay_us (CPU cycle counter)
 * and every wait uses the FreeRTOS tick (SYSTIMER UNIT1, unaffected). The only
 * esp_timer reads are the backstep checks themselves.
 *
 * Wiring (XIAO ESP32-C6): TRIG = D6 (GPIO16), ECHO = D7 (GPIO17), sensor on
 * 3V3. D6/D7 are UART0 TX/RX; free because the console is USB-Serial-JTAG. The
 * ROM prints its boot banner on GPIO16, so expect a few junk pings at power-up.
 * A 5 V-only HC-SR04 needs a divider on ECHO; the RCWL-1601 runs at 3.3 V. */
#ifndef REPRO_ULTRASONIC
#define REPRO_ULTRASONIC 0
#endif
#if REPRO_ULTRASONIC && REPRO_I2C
#error "REPRO_ULTRASONIC replaces the I2C load: build with -DREPRO_I2C=0"
#endif
#if REPRO_ULTRASONIC
#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "esp_rom_sys.h"
#include "freertos/queue.h"
#ifndef REPRO_US_TRIG_GPIO
#define REPRO_US_TRIG_GPIO 16          /* XIAO C6 D6 */
#endif
#ifndef REPRO_US_ECHO_GPIO
#define REPRO_US_ECHO_GPIO 17          /* XIAO C6 D7 */
#endif
/* Ping cadence. Default 1 Hz mirrors the field sensor's 1 s update, so the
 * peripheral sees the same active-then-idle duty the I2C runs used. */
#ifndef REPRO_US_PERIOD_MS
#define REPRO_US_PERIOD_MS 1000
#endif
#ifndef REPRO_IDLE_CHUNK_MS
#define REPRO_IDLE_CHUNK_MS 50
#endif
#define US_ECHO_TIMEOUT_MS 80          /* no edge within this -> "no echo" */
#define US_MAX_ECHO_US     26000       /* ~4.5 m; longer = nothing in range */

static volatile uint64_t s_us_ok;      /* valid echoes */
static volatile uint64_t s_us_err;     /* no echo at all (wiring/sensor fault) */
static volatile uint64_t s_us_far;     /* echo, but nothing in range */
static volatile uint32_t s_us_last_mm;
static rmt_channel_handle_t s_us_rx;
static QueueHandle_t s_us_q;
static rmt_symbol_word_t s_us_sym[64];

static bool IRAM_ATTR us_rx_done(rmt_channel_handle_t ch,
                                 const rmt_rx_done_event_data_t *ed, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xQueueSendFromISR(s_us_q, ed, &woken);
    return woken == pdTRUE;
}

static void us_report_backstep(int64_t before_us, int64_t after_us, int kind, int idx)
{
    int64_t back = after_us - before_us;           /* negative */
    ESP_LOGE(TAG, "*** BACKSTEP DURING %s (idx %d) ***", PHASE_NAME[kind], idx);
    ESP_LOGE(TAG, "    esp_timer %lld -> %lld us  (moved %lld us = %.2f min)",
             (long long)before_us, (long long)after_us, (long long)back,
             (double)back / 60000000.0);
    ESP_LOGE(TAG, "    US SEQ: ok=%llu err=%llu far=%llu last=%" PRIu32 " mm",
             (unsigned long long)s_us_ok, (unsigned long long)s_us_err,
             (unsigned long long)s_us_far, s_us_last_mm);
    ESP_LOGE(TAG, "    tick=%" PRIu32 " ms  UNIT0=%llu UNIT1=%llu  CONF=0x%08" PRIx32,
             (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
             (unsigned long long)unit0_raw(), (unsigned long long)unit1_raw(),
             (uint32_t)SYSTIMER_CONF);
    report_raw();
    persist_phase(kind, idx, back, s_us_ok);
}

static void us_init(void)
{
    gpio_reset_pin(REPRO_US_TRIG_GPIO);            /* detach UART0 TX */
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << REPRO_US_TRIG_GPIO,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(REPRO_US_TRIG_GPIO, 0);

    rmt_rx_channel_config_t rx = {
        .gpio_num = REPRO_US_ECHO_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,                  /* 1 tick = 1 us */
        .mem_block_symbols = 48,
    };
    ESP_ERROR_CHECK(rmt_new_rx_channel(&rx, &s_us_rx));
    s_us_q = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
    rmt_rx_event_callbacks_t cbs = { .on_recv_done = us_rx_done };
    ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(s_us_rx, &cbs, NULL));
    ESP_ERROR_CHECK(rmt_enable(s_us_rx));
}

/* One ping. Returns echo width in us, 0 = no echo (timed out). */
static uint32_t us_ping(void)
{
    const rmt_receive_config_t rc = {
        .signal_range_min_ns = 1000,               /* drop glitches < 1 us */
        .signal_range_max_ns = 30 * 1000 * 1000,   /* frame ends after 30 ms flat */
    };
    xQueueReset(s_us_q);
    if (rmt_receive(s_us_rx, s_us_sym, sizeof(s_us_sym), &rc) != ESP_OK)
        return 0;
    gpio_set_level(REPRO_US_TRIG_GPIO, 1);
    esp_rom_delay_us(10);
    gpio_set_level(REPRO_US_TRIG_GPIO, 0);

    rmt_rx_done_event_data_t ev;
    if (xQueueReceive(s_us_q, &ev, pdMS_TO_TICKS(US_ECHO_TIMEOUT_MS)) != pdTRUE) {
        rmt_disable(s_us_rx);                      /* abandon the pending receive */
        rmt_enable(s_us_rx);
        return 0;
    }
    /* First high segment = echo width. */
    for (size_t i = 0; i < ev.num_symbols; i++) {
        const rmt_symbol_word_t *w = &ev.received_symbols[i];
        if (w->level0 == 1 && w->duration0) return w->duration0;
        if (w->level1 == 1 && w->duration1) return w->duration1;
    }
    return 0;
}

static void us_task(void *arg)
{
    int64_t t_prev = esp_timer_get_time();
    uint32_t logged = 0;
    for (;;) {
        int64_t t_pre = esp_timer_get_time();
        if (t_pre < t_prev) us_report_backstep(t_prev, t_pre, PH_WAKE, 0);

        uint32_t w = us_ping();

        int64_t t_post = esp_timer_get_time();
        if (t_post < t_pre) us_report_backstep(t_pre, t_post, PH_PING, 0);

        if (w == 0) {
            if ((s_us_err++ % 100) == 0)
                ESP_LOGW(TAG, "ultrasonic: no echo (total %llu) -- check wiring/power",
                         (unsigned long long)s_us_err);
        } else if (w >= US_MAX_ECHO_US) {
            s_us_far++;
        } else {
            s_us_ok++;
            s_us_last_mm = (w * 1715U) / 10000U;   /* 343 m/s, round trip */
        }
        if (logged < 5) {                          /* prove the sensor works */
            logged++;
            ESP_LOGW(TAG, "ultrasonic ping %" PRIu32 ": echo %" PRIu32 " us -> %" PRIu32 " mm",
                     logged, w, w ? (w * 1715U) / 10000U : 0);
        }

        /* Idle in chunks so a step inside the idle window is caught promptly. */
        int64_t t_idle = t_post;
        for (int k = 0; k < (REPRO_US_PERIOD_MS / REPRO_IDLE_CHUNK_MS); k++) {
            vTaskDelay(pdMS_TO_TICKS(REPRO_IDLE_CHUNK_MS));
            int64_t t_now = esp_timer_get_time();
            if (t_now < t_idle) us_report_backstep(t_idle, t_now, PH_IDLE, k);
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
#if REPRO_WITNESS
    uint64_t prev_gpt_us = gpt_us();
#endif

#if REPRO_I2C
    ESP_LOGI(TAG, "monitoring. SYSTIMER_CONF=0x%08" PRIx32 "  I2C ACTIVE, period %d ms, pullups=%s",
             (uint32_t)SYSTIMER_CONF, REPRO_I2C_PERIOD_MS,
             REPRO_INTERNAL_PULLUP ? "INTERNAL ~45k (weak, pre-08-21 config)" : "external 4.7k");
    ESP_LOGI(TAG, "bus speed %d Hz  (field ran 100000 during frequent failures)", I2C_HZ);
    ESP_LOGI(TAG, "duty pattern: %d transfers per burst, %d ms idle between "
                  "(field-like; hammering may SUPPRESS an idle->active trigger)",
             REPRO_BURST, REPRO_IDLE_MS);
#elif REPRO_ULTRASONIC
    ESP_LOGI(TAG, "monitoring. SYSTIMER_CONF=0x%08" PRIx32 "  I2C OFF, ULTRASONIC trig=GPIO%d echo=GPIO%d, ping every %d ms (RMT-timed)",
             (uint32_t)SYSTIMER_CONF, REPRO_US_TRIG_GPIO, REPRO_US_ECHO_GPIO, REPRO_US_PERIOD_MS);
#else
    ESP_LOGI(TAG, "monitoring. SYSTIMER_CONF=0x%08" PRIx32 "  I2C DISABLED (control build)",
             (uint32_t)SYSTIMER_CONF);
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(period_ms));

#if REPRO_WITNESS
        /* Sampled together so a preemption between reads can't fake a lag. */
        static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
        portENTER_CRITICAL(&mux);
#endif
        int64_t  esp_us  = esp_timer_get_time();
        uint32_t tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
#if REPRO_WITNESS
        uint64_t g_us    = gpt_us();
        portEXIT_CRITICAL(&mux);
#endif

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
            report_raw();
            ESP_LOGE(TAG, "    event #%" PRIu32 ", worst so far %lld us", events, (long long)worst_back_us);
            persist_backstep(d_esp_us, (int64_t)tick_ms - esp_us / 1000,
#if REPRO_I2C
                             s_i2c_ok);
#elif REPRO_ULTRASONIC
                             s_us_ok);
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

#if REPRO_WITNESS
        {
            int64_t d_gpt_us = (int64_t)(g_us - prev_gpt_us);
            int64_t lost1_ms = d_gpt_us / 1000 - (int64_t)d_tick_ms;
            int64_t lost0_ms = (d_gpt_us - d_esp_us) / 1000;
            if (lost1_ms >= WITNESS_MS || lost1_ms <= -WITNESS_MS) {
                s_witness_events++;
                ESP_LOGE(TAG, "*** WITNESS: UNIT1 (tick) %s %lld ms: tick +%" PRIu32 " ms, GPTimer +%lld us ***",
                         lost1_ms > 0 ? "LOST" : "GAINED", (long long)(lost1_ms > 0 ? lost1_ms : -lost1_ms),
                         d_tick_ms, (long long)d_gpt_us);
                report_raw();
            }
            /* A backward step was reported above; this adds the short and forward cases. */
            if (d_esp_us >= 0 && (lost0_ms >= WITNESS_MS || lost0_ms <= -WITNESS_MS)) {
                s_witness_events++;
                ESP_LOGE(TAG, "*** WITNESS: UNIT0 (esp_timer) %s %lld ms: esp_timer +%lld us, GPTimer +%lld us ***",
                         lost0_ms > 0 ? "LOST" : "GAINED", (long long)(lost0_ms > 0 ? lost0_ms : -lost0_ms),
                         (long long)d_esp_us, (long long)d_gpt_us);
                report_raw();
            }
            prev_gpt_us = g_us;
        }
#endif
        prev_esp_us  = esp_us;
        prev_tick_ms = tick_ms;

#if REPRO_TIMEWARP && REPRO_REWARP_HOURS
        if (tick_ms > (uint32_t)REPRO_REWARP_HOURS * 3600U * 1000U) {
            ESP_LOGW(TAG, "REWARP: %u h elapsed on tick clock; rebooting to re-apply the preset "
                          "before natural carry erodes bits 41..50.", (unsigned)REPRO_REWARP_HOURS);
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
#endif
        /* Heartbeat once a minute: esp_timer vs tick, and the running deficit. */
        static uint32_t n;
        if (++n % (60000 / period_ms) == 0) {
            int64_t esp_ms = esp_us / 1000;
            /* tick time, not esp_timer: the uptime has to survive a backstep */
            if ((tick_ms / 60000U) % UPTIME_SAVE_MIN == 0)
                persist_uptime(tick_ms / 60000U);
            /* The LOAD canary is deliberately NOT polled here: by inspection
             * nothing writes those registers after boot, and every extra
             * SYSTIMER access is a perturbation. report_raw() reads it at
             * event time and flags an overwrite then. */
#if REPRO_WITNESS
            /* Cumulative since the baseline; both should sit near 0 ms. */
            ESP_LOGI(TAG, "  witness GPTimer %llu ms | UNIT0-gpt %+lld ms | UNIT1-gpt %+lld ms | witness events %" PRIu32,
                     (unsigned long long)(g_us / 1000),
                     (long long)(((esp_us - s_base_esp_us) - (int64_t)(g_us - s_base_gpt_us)) / 1000),
                     (long long)((int64_t)(tick_ms - s_base_tick_ms) - (int64_t)(g_us - s_base_gpt_us) / 1000),
                     s_witness_events);
#endif
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
#elif REPRO_ULTRASONIC
            ESP_LOGI(TAG, "esp_timer %lld ms | tick %" PRIu32 " ms | deficit %lld ms | backsteps %"
                          PRIu32 " | us ok=%llu err=%llu far=%llu last=%" PRIu32 " mm",
                     (long long)esp_ms, tick_ms, (long long)((int64_t)tick_ms - esp_ms), events,
                     (unsigned long long)s_us_ok, (unsigned long long)s_us_err,
                     (unsigned long long)s_us_far, s_us_last_mm);
#if REPRO_WIFI
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
#if REPRO_TIMEWARP
    {
        /* Low offset (2026-09-04): two events hit at boot age 11,816 s +-1 s
         * (same idle chunk) on different boots. Boot-age and counter-content
         * are the same variable when the preset only touches bits 41+, so a
         * scheduled-timer trigger and a content trigger are indistinguishable.
         * Adding 2^33 (8.95 min) to the preset detaches them: a timer twin
         * recurs at 11,816 s boot age; a content twin recurs 537 s EARLIER. */
        const uint64_t warp_ticks = (1ULL<<51) - (1ULL<<41) + (1ULL<<33);
        const uint64_t warp_us    = warp_ticks / 16ULL;   /* exact: both /16 */
        esp_timer_private_set(warp_us);
        ESP_LOGW(TAG, "TIMEWARP: esp_timer preset to %llu us (~8.9 yr): counter bits 41..50 + 33 SET (low offset 2^33 for the timer-vs-content discriminator)",
                 (unsigned long long) warp_us);
        ESP_LOGW(TAG, "TIMEWARP: high-bit observation window ~38 h until natural carry; UNIT1-UNIT0 baseline is now -(preset)");
    }
#endif
    /* After the preset: esp_timer_private_set() goes through the same load
     * registers and would overwrite the canary. */
    load_canary_arm();
#if REPRO_WITNESS
    /* Before any task that can call report_raw(). */
    witness_start();
#endif
#if REPRO_WIFI
    wifi_start();
#endif
    ESP_LOGW(TAG, "boot #%" PRIu32 " | lifetime backsteps %" PRIu32 " | worst %lld us",
             s_boot_count, s_total_backsteps, (long long)s_worst_back_us_persist);
    ESP_LOGW(TAG, "reset reason %s | previous boot ran >= %" PRIu32 " min",
             reset_reason_name(esp_reset_reason()), s_prev_up_min);
    {
        nvs_handle_t h; char hist[200] = ""; size_t hl = sizeof(hist);
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            nvs_get_str(h, "rst_hist", hist, &hl);
            nvs_close(h);
        }
        ESP_LOGW(TAG, "reset history (boot:reason:prev_min, newest first): %s", hist);
    }
    {   /* Report the phase of the last recorded backstep. Survives the
         * panic-reboot that cost us the 08-26 event's context entirely. */
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
            int32_t k = -1, ix = -1; int64_t bk = 0; uint64_t tx = 0;
            nvs_get_i32(h, "ph_kind", &k); nvs_get_i32(h, "ph_idx", &ix);
            nvs_get_i64(h, "ph_back", &bk); nvs_get_u64(h, "ph_tx", &tx);
            if (k >= 0 && k < PH_COUNT)
                ESP_LOGW(TAG, "LAST BACKSTEP PHASE: %s idx=%" PRId32
                              " back=%lld us at tx #%llu",
                         PHASE_NAME[k], ix, (long long)bk,
                         (unsigned long long)tx);
            else
                ESP_LOGW(TAG, "no phase-resolved backstep recorded yet");
            uint64_t u0, u1, l0, l1; uint32_t rt, rb;
            if (nvs_get_u64(h, "raw_u0", &u0) == ESP_OK && nvs_get_u64(h, "raw_u1", &u1) == ESP_OK &&
                nvs_get_u64(h, "raw_l0", &l0) == ESP_OK && nvs_get_u64(h, "raw_l1", &l1) == ESP_OK &&
                nvs_get_u32(h, "raw_tick", &rt) == ESP_OK && nvs_get_u32(h, "raw_boot", &rb) == ESP_OK)
                ESP_LOGW(TAG, "LAST RAW CAPTURE (boot #%" PRIu32 ", tick %" PRIu32 " ms): UNIT0=0x%013llx "
                              "UNIT1=0x%013llx LOAD0=0x%013llx LOAD1=0x%013llx",
                         rb, rt, (unsigned long long)u0, (unsigned long long)u1,
                         (unsigned long long)l0, (unsigned long long)l1);
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
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_bus));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = I2C_ADDR,
        .scl_speed_hz = I2C_HZ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev));

    xTaskCreate(i2c_task, "i2c", 4096, NULL, 5, NULL);
#endif
#if REPRO_ULTRASONIC
    us_init();
    xTaskCreate(us_task, "us", 4096, NULL, 5, NULL);
#endif
    xTaskCreate(monitor_task, "monitor", 4096, NULL, 4, NULL);
}
