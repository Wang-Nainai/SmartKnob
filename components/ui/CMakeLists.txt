idf_component_register(SRCS "smartknob_ui.c"
                       "pages/pg_startup.c"
                       "pages/pg_menu.c"
                       "pages/pg_pcdial.c"
                       "pages/pg_playground.c"
                       "pages/pg_hass.c"
                       "pages/pg_env.c"
                       "pages/pg_setting.c"
                       "pages/pg_sysinfo.c"
                       "pages/pg_factory.c"
                       "pages/pg_apcfg.c"
                       "fonts/lv_font_msyh_16.c"
                       INCLUDE_DIRS include
                       REQUIRES lvgl__lvgl display mqtt_ha motor wifi esp_app_format esp_timer nvs_flash input led app_state blehid)