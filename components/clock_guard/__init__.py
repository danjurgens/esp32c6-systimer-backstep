# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Dan Jurgens
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import (
    CONF_ID,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_MILLISECOND,
    UNIT_SECOND,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

CODEOWNERS = ["@dan"]
DEPENDENCIES = []
AUTO_LOAD = ["sensor"]

clock_guard_ns = cg.esphome_ns.namespace("clock_guard")
ClockGuard = clock_guard_ns.class_("ClockGuard", cg.Component)

CONF_CHECK_INTERVAL = "check_interval"
CONF_THRESHOLD = "threshold"
CONF_MAX_CORRECTION = "max_correction"
CONF_CORRECTION_COUNT = "correction_count"
CONF_LAST_CORRECTION = "last_correction"
CONF_TOTAL_CORRECTED = "total_corrected"
CONF_DIVERGENCE = "divergence"


def _validate(config):
    # The whole point is to repair BEFORE loop_watchdog reboots. If the check
    # interval approaches the watchdog's timeout the guard is useless, so keep
    # it far below. 1 s against a 35 s timeout gives 35x headroom.
    if config[CONF_CHECK_INTERVAL].total_milliseconds > 5000:
        raise cv.Invalid(
            "check_interval must be <= 5s: the guard has to correct the clock "
            "before loop_watchdog's timeout expires, or it serves no purpose."
        )
    # Below ~2s of divergence we are looking at tick jitter, not the fault.
    # The observed fault is TENS OF MINUTES; a low threshold risks correcting
    # noise and fighting the tick.
    if config[CONF_THRESHOLD].total_milliseconds < 2000:
        raise cv.Invalid(
            "threshold must be >= 2s. Observed backward steps are tens of "
            "minutes; anything smaller is tick jitter and must not be 'repaired'."
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(ClockGuard),
            cv.Optional(CONF_CHECK_INTERVAL, default="1s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_THRESHOLD, default="5s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_MAX_CORRECTION, default="2h"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_CORRECTION_COUNT): sensor.sensor_schema(
                accuracy_decimals=0,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-check-outline",
            ),
            cv.Optional(CONF_LAST_CORRECTION): sensor.sensor_schema(
                unit_of_measurement=UNIT_MILLISECOND,
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-fast",
            ),
            cv.Optional(CONF_TOTAL_CORRECTED): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                accuracy_decimals=1,
                state_class=STATE_CLASS_TOTAL_INCREASING,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-plus-outline",
            ),
            cv.Optional(CONF_DIVERGENCE): sensor.sensor_schema(
                unit_of_measurement=UNIT_MILLISECOND,
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:clock-alert-outline",
            ),
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _validate,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_check_interval_ms(config[CONF_CHECK_INTERVAL].total_milliseconds))
    cg.add(var.set_threshold_ms(config[CONF_THRESHOLD].total_milliseconds))
    cg.add(var.set_max_correction_ms(config[CONF_MAX_CORRECTION].total_milliseconds))

    for key, setter in (
        (CONF_CORRECTION_COUNT, var.set_correction_count_sensor),
        (CONF_LAST_CORRECTION, var.set_last_correction_sensor),
        (CONF_TOTAL_CORRECTED, var.set_total_corrected_sensor),
        (CONF_DIVERGENCE, var.set_divergence_sensor),
    ):
        if key in config:
            s = await sensor.new_sensor(config[key])
            cg.add(setter(s))
