#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_check.h"
#include "scd40.h"

static const char *TAG = "scd40";

#define SCD40_CMD_START_PERIODIC 0x21B1
#define SCD40_CMD_STOP_PERIODIC  0x3F86
#define SCD40_CMD_DATA_READY     0xE4B8
#define SCD40_CMD_READ_MEAS      0xEC05
#define SCD40_CMD_REINIT         0x3646   /* 仅怠速态合法: 复位内部状态, 20ms */

#define SCD40_FIRST_MEASUREMENT_DELAY_MS 5000

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

static uint8_t scd40_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static esp_err_t scd40_write_cmd(uint16_t cmd)
{
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)cmd };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static void scd40_scan_bus(void)
{
    ESP_LOGI(TAG, "Scanning I2C bus");
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(s_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "  Device found at 0x%02X", addr);
        }
    }
    ESP_LOGI(TAG, "Scan complete");
}

static bool scd40_verify_crc(const uint8_t *buf, size_t word_count)
{
    for (size_t i = 0; i < word_count; i++) {
        if (buf[i * 3 + 2] != scd40_crc8(&buf[i * 3], 2)) {
            return false;
        }
    }
    return true;
}

esp_err_t scd40_init(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = CONFIG_SCD40_SDA_PIN,
        .scl_io_num = CONFIG_SCD40_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_bus), TAG, "I2C bus init failed");
    ESP_LOGI(TAG, "I2C bus initialized (SDA=%d, SCL=%d)", CONFIG_SCD40_SDA_PIN, CONFIG_SCD40_SCL_PIN);

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = SCD40_I2C_ADDR,
        .scl_speed_hz = CONFIG_SCD40_I2C_SPEED_HZ,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev), TAG, "add SCD40 device failed");

    /* SCD4x datasheet: 上电后需等待 >=1000ms 才能接受 start_periodic,
     * 20ms 就下发 start 会被静默忽略 (表现为持续 not ready, 只能靠 60s
     * 自愈循环拉回来) */
    vTaskDelay(pdMS_TO_TICKS(1000));
    scd40_scan_bus();
    return ESP_OK;
}

esp_err_t scd40_start_periodic(void)
{
    ESP_LOGI(TAG, "Starting periodic measurement");
    esp_err_t err = scd40_write_cmd(SCD40_CMD_START_PERIODIC);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start periodic failed, err=0x%X", err);
    }
    return err;
}

esp_err_t scd40_stop_periodic(void)
{
    return scd40_write_cmd(SCD40_CMD_STOP_PERIODIC);
}

/* 测量重启序列: 停止 -> 等退出测量态 -> reinit 清内部状态 -> 重新启动.
 * 软重启时传感器可能带着上一轮的测量状态, 直接 start 会被忽略
 * (表现为 get_data_ready 恒返回未就绪) —— 此序列强制拉回正常测量 */
esp_err_t scd40_restart_measurement(void)
{
    esp_err_t err = scd40_stop_periodic();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "stop failed (0x%X), trying start anyway", err);
    }
    vTaskDelay(pdMS_TO_TICKS(800));
    err = scd40_write_cmd(SCD40_CMD_REINIT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "reinit failed (0x%X), retry start anyway", err);
    } else {
        vTaskDelay(pdMS_TO_TICKS(30));
    }
    return scd40_start_periodic();
}

esp_err_t scd40_data_ready(bool *ready)
{
    /* 计数移到最前: 持续性 I2C 总线错误(而非"未就绪")也计入,
     * 使自愈流程同样覆盖总线级故障。
     * 连续自愈失败 2 次后间隔从 60s 退避到 180s —— 不反复 stop/reinit
     * 折腾传感器(实测: 传感器状态异常时自愈拉不回, 需硬件排查) */
    static uint32_t not_ready_count = 0;
    static int heal_fails = 0;
    int heal_after = (heal_fails >= 2) ? 90 : 30;
    uint8_t cmd[2] = { SCD40_CMD_DATA_READY >> 8, SCD40_CMD_DATA_READY & 0xFF };
    uint8_t buf[3];
    esp_err_t err = i2c_master_transmit_receive(s_dev, cmd, sizeof(cmd), buf, sizeof(buf), 200);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "data-ready tx/rx failed, err=0x%X", err);
        if (not_ready_count++ >= heal_after) {
            ESP_LOGW(TAG, "no data (bus err) for %ds, recovering sensor", heal_after * 2);
            scd40_restart_measurement();
            if (++heal_fails > 3) heal_fails = 2;
            not_ready_count = 0;
        }
        return err;
    }
    if (!scd40_verify_crc(buf, 1)) {
        ESP_LOGW(TAG, "data-ready CRC mismatch: [0x%02X,0x%02X,0x%02X]", buf[0], buf[1], buf[2]);
        return ESP_ERR_INVALID_CRC;
    }
    uint16_t status = (uint16_t)((buf[0] << 8) | buf[1]);
    *ready = (status & 0x7FF) != 0;  /* SCD4x: least-significant 11 bits non-zero => data ready */
    if (!*ready) {
        if (not_ready_count++ % 5 == 0) {
            ESP_LOGI(TAG, "data not ready, status=0x%04X", status);
        }
        if (not_ready_count == heal_after) {
            /* 无数据达到阈值: 传感器自愈 (软重启后 start 被忽略等状态) */
            ESP_LOGW(TAG, "no data for %ds, recovering sensor (stop->reinit->start)",
                     heal_after * 2);
            scd40_restart_measurement();
            if (++heal_fails >= 2 && heal_after == 30) {
                ESP_LOGW(TAG, "self-heal failing repeatedly, backing off to 3min interval");
            }
            if (heal_fails > 3) heal_fails = 3;
            not_ready_count = 0;
        }
    } else {
        not_ready_count = 0;
        heal_fails = 0;
    }
    return ESP_OK;
}

esp_err_t scd40_read(scd40_data_t *data)
{
    uint8_t cmd[2] = { SCD40_CMD_READ_MEAS >> 8, SCD40_CMD_READ_MEAS & 0xFF };
    uint8_t buf[9];
    esp_err_t err = i2c_master_transmit_receive(s_dev, cmd, sizeof(cmd), buf, sizeof(buf), 200);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "read-meas tx/rx failed, err=0x%X", err);
        return err;
    }
    if (!scd40_verify_crc(buf, 3)) {
        ESP_LOGW(TAG, "Measurement CRC mismatch: [0x%02X,0x%02X,0x%02X]", buf[0], buf[1], buf[2]);
        return ESP_ERR_INVALID_CRC;
    }

    uint16_t co2 = (uint16_t)((buf[0] << 8) | buf[1]);
    uint16_t t_raw = (uint16_t)((buf[3] << 8) | buf[4]);
    uint16_t rh_raw = (uint16_t)((buf[6] << 8) | buf[7]);

    data->co2_ppm = co2;
    data->temperature_c = -45.0f + 175.0f * (float)t_raw / 65535.0f;
    data->humidity_pct = 100.0f * (float)rh_raw / 65535.0f;

    ESP_LOGI(TAG, "CO2=%u ppm, T=%.1f C, RH=%.1f %%", co2, data->temperature_c, data->humidity_pct);
    return ESP_OK;
}