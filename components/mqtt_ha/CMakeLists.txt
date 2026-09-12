idf_component_register(SRCS "mqtt.c"
                    INCLUDE_DIRS "."
                    REQUIRES mqtt driver webcfg led motor hass_cfg)