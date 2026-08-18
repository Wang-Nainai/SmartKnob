idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES led wifi display ui scd40 mqtt_ha motor webcfg input app_state)
