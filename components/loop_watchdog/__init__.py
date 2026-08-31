# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Jurgens
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    CONF_TIMEOUT,
    CONF_UPDATE_INTERVAL,
    UNIT_MILLISECOND,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

CODEOWNERS = ["@dan"]
DEPENDENCIES = []
AUTO_LOAD = ["sensor"]

loop_watchdog_ns = cg.esphome_ns.namespace("loop_watchdog")
LoopWatchdog = loop_watchdog_ns.class_("LoopWatchdog", cg.PollingComponent)

CONF_REBOOT = "reboot"
CONF_REQUIRE_CLOCK_STALL = "require_clock_stall"
CONF_CLOCK_STALL_THRESHOLD = "clock_stall_threshold"
CONF_REBOOT_AFTER = "reboot_after"
CONF_MICRO_STALL_THRESHOLD = "micro_stall_threshold"
CONF_CLOCK_DIVERGENCE = "clock_divergence"
CONF_TICK_UPTIME = "tick_uptime"
CONF_STALL_COUNT = "stall_count"
CONF_LAST_STALL_DURATION = "last_stall_duration"
CONF_LAST_STALL_DIVERGENCE = "last_stall_divergence"
CONF_REBOOT_COUNT = "reboot_count"
CONF_MICRO_STALL_COUNT = "micro_stall_count"
CONF_TOTAL_STALLED = "total_stalled"


def _validate(config):
    interval = config[CONF_UPDATE_INTERVAL].total_milliseconds
    timeout = config[CONF_TIMEOUT].total_milliseconds
    if timeout <= interval * 3:
        raise cv.Invalid(
            f"timeout ({timeout} ms) must be more than 3x update_interval "
            f"({interval} ms), or normal jitter will look like a stall"
        )
    stall = config[CONF_CLOCK_STALL_THRESHOLD].total_milliseconds
    if config[CONF_REQUIRE_CLOCK_STALL] and stall >= timeout:
        raise cv.Invalid(
            f"clock_stall_threshold ({stall} ms) must be less than timeout "
            f"({timeout} ms), or the clock check can never be satisfied"
        )
    after = config[CONF_REBOOT_AFTER].total_milliseconds
    if after != 0 and after < timeout:
        raise cv.Invalid(
            f"reboot_after ({after} ms) must be 0 or >= timeout ({timeout} ms)"
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LoopWatchdog),
            # update() starvation that counts as a stall.
            cv.Optional(CONF_TIMEOUT, default="120s"): cv.positive_time_period_milliseconds,
            # False = record stalls but never reboot. Use on a testbed you are
            # deliberately trying to catch in the failed state.
            cv.Optional(CONF_REBOOT, default=True): cv.boolean,
            # Reboot only if the tick and esp_timer have ALSO diverged. This is
            # the OTA interlock: an OTA blocks the loop (starving update()) but
            # esp_timer keeps running, so divergence stays ~0 and we hold fire.
            cv.Optional(CONF_REQUIRE_CLOCK_STALL, default=True): cv.boolean,
            cv.Optional(
                CONF_CLOCK_STALL_THRESHOLD, default="30s"
            ): cv.positive_time_period_milliseconds,
            # Backstop: reboot after this much stall even if the clock looks
            # fine. Covers a scheduler stall whose cause is NOT the clock.
            # Set well above your longest OTA. 0 disables.
            cv.Optional(
                CONF_REBOOT_AFTER, default="10min"
            ): cv.positive_time_period_milliseconds,
            # A jump in clock divergence between two consecutive update()
            # calls larger than this counts as a micro-stall: esp_timer lost
            # time, but for less than `timeout`, so the watchdog never saw it.
            # These are invisible without this check, and they are what would
            # distinguish "one rare long freeze" from "a constant drizzle".
            cv.Optional(
                CONF_MICRO_STALL_THRESHOLD, default="2s"
            ): cv.positive_time_period_milliseconds,
            # --- telemetry ---
            # Continuous: signed (tick - esp_timer) in ms. Healthy is flat and
            # near zero. A sustained climb is the scheduler clock dying.
            cv.Optional(CONF_CLOCK_DIVERGENCE): sensor.sensor_schema(
                unit_of_measurement=UNIT_MILLISECOND,
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-alert-outline",
            ),
            # Tick-based uptime (xTaskGetTickCount). ESPHome's built-in uptime
            # sensor publishes millis_64() = esp_timer, which is the clock that
            # freezes. Graph both: a stall is esp_timer uptime going flat while
            # this keeps climbing.
            cv.Optional(CONF_TICK_UPTIME): sensor.sensor_schema(
                unit_of_measurement="s",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-check-outline",
            ),
            # NVS-backed, so they survive reboots AND power cycles.
            # stall_count counts EVERY stall, including ones that recovered on
            # their own with no reboot -- which otherwise leave no trace.
            cv.Optional(CONF_STALL_COUNT): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:timer-alert-outline",
            ),
            cv.Optional(CONF_REBOOT_COUNT): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:restart-alert",
            ),
            # Stalls too short to trip the watchdog. If this climbs while
            # stall_count stays flat, the fault is frequent and small rather
            # than rare and large.
            cv.Optional(CONF_MICRO_STALL_COUNT): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:pulse",
            ),
            # Lifetime esp_timer time lost, in seconds. NVS-backed, so it
            # survives the reboots the watchdog itself causes -- otherwise
            # every trip would reset the record to zero.
            cv.Optional(CONF_TOTAL_STALLED): sensor.sensor_schema(
                unit_of_measurement="s",
                accuracy_decimals=1,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-minus-outline",
            ),
            cv.Optional(CONF_LAST_STALL_DURATION): sensor.sensor_schema(
                unit_of_measurement=UNIT_MILLISECOND,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:timer-sand",
            ),
            cv.Optional(CONF_LAST_STALL_DIVERGENCE): sensor.sensor_schema(
                unit_of_measurement=UNIT_MILLISECOND,
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-remove-outline",
            ),
        }
    ).extend(cv.polling_component_schema("10s")),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_timeout_ms(config[CONF_TIMEOUT]))
    cg.add(var.set_reboot(config[CONF_REBOOT]))
    cg.add(var.set_require_clock_stall(config[CONF_REQUIRE_CLOCK_STALL]))
    cg.add(var.set_clock_stall_ms(config[CONF_CLOCK_STALL_THRESHOLD]))
    cg.add(var.set_reboot_after_ms(config[CONF_REBOOT_AFTER]))
    cg.add(var.set_micro_stall_ms(config[CONF_MICRO_STALL_THRESHOLD]))

    for key, setter in (
        (CONF_CLOCK_DIVERGENCE, var.set_divergence_sensor),
        (CONF_TICK_UPTIME, var.set_tick_uptime_sensor),
        (CONF_STALL_COUNT, var.set_stall_count_sensor),
        (CONF_LAST_STALL_DURATION, var.set_last_stall_duration_sensor),
        (CONF_LAST_STALL_DIVERGENCE, var.set_last_stall_divergence_sensor),
        (CONF_REBOOT_COUNT, var.set_reboot_count_sensor),
        (CONF_MICRO_STALL_COUNT, var.set_micro_stall_count_sensor),
        (CONF_TOTAL_STALLED, var.set_total_stalled_sensor),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(setter(s))
