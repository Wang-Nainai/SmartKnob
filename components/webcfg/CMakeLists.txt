idf_component_register(SRCS "webcfg.c"
                       INCLUDE_DIRS include
                       REQUIRES esp_http_server nvs_flash esp_app_format esp_timer app_update app_state motor blehid display led esp_wifi)