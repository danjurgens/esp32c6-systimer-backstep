idf_component_register(SRCS "repro_main.c" REQUIRES driver esp_timer nvs_flash esp_wifi esp_event esp_netif)

# idf.py -D sets a CMAKE CACHE VARIABLE, not a preprocessor define. Without
# this plumbing the flags are silently ignored and every build uses the
# defaults -- which is exactly what happened, mislabelling several runs.
foreach(opt REPRO_I2C REPRO_I2C_HZ REPRO_I2C_PERIOD_MS REPRO_INTERNAL_PULLUP REPRO_WIFI REPRO_BURST REPRO_IDLE_MS REPRO_IDLE_CHUNK_MS REPRO_MODEM_SLEEP REPRO_TIMEWARP REPRO_REWARP_HOURS REPRO_RANGE_CHECK)
    if(DEFINED ${opt})
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${opt}=${${opt}})
        message(STATUS "repro: ${opt}=${${opt}}")
    endif()
endforeach()
