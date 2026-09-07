#include "blehid.h"
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "blehid";

#define HID_DEV_NAME        "SmartKnob"
#define HID_APPEARANCE      0x03C2      /* Generic HID */

/* ---------------- HID Report Map ----------------
 * Report ID 1: Consumer Control (16bit usage 数组)
 * Report ID 2: Mouse (3 按键 + XY 相对 + 滚轮)
 * Windows/macOS/Linux 免驱识别。 */
static const uint8_t s_report_map[] = {
    /* Mouse */
    0x05, 0x01,                     /* Usage Page (Generic Desktop) */
    0x09, 0x02,                     /* Usage (Mouse) */
    0xA1, 0x01,                     /* Collection (Application) */
    0x85, 0x02,                     /*   Report ID (2) */
    0x09, 0x01,                     /*   Usage (Pointer) */
    0xA1, 0x00,                     /*   Collection (Physical) */
    0x05, 0x09,                     /*     Usage Page (Buttons) */
    0x19, 0x01, 0x29, 0x03,         /*     Usage Min 1, Max 3 */
    0x15, 0x00, 0x25, 0x01,         /*     Logical 0..1 */
    0x75, 0x01, 0x95, 0x03,         /*     Size 1, Count 3 */
    0x81, 0x02,                     /*     Input (Data, Var, Abs) */
    0x75, 0x05, 0x95, 0x01,         /*     padding: Size 5, Count 1 */
    0x81, 0x03,                     /*     Input (Const, Var, Abs) */
    0x05, 0x01,                     /*     Usage Page (Generic Desktop) */
    0x09, 0x30, 0x09, 0x31,         /*     Usage X, Y */
    0x09, 0x38,                     /*     Usage (Wheel) */
    0x15, 0x81, 0x25, 0x7F,         /*     Logical -127..127 */
    0x75, 0x08, 0x95, 0x03,         /*     Size 8, Count 3 */
    0x81, 0x06,                     /*     Input (Data, Var, Rel) */
    0xC0, 0xC0,                     /*   End Collection x2 */
    /* Consumer Control */
    0x05, 0x0C,                     /* Usage Page (Consumer) */
    0x09, 0x01,                     /* Usage (Consumer Control) */
    0xA1, 0x01,                     /* Collection (Application) */
    0x85, 0x01,                     /*   Report ID (1) */
    0x15, 0x00,                     /*   Logical Min 0 */
    0x26, 0xFF, 0x03,               /*   Logical Max 1023 */
    0x19, 0x00,                     /*   Usage Min 0 */
    0x2A, 0xFF, 0x03,               /*   Usage Max 1023 */
    0x75, 0x10, 0x95, 0x01,         /*   Size 16, Count 1 */
    0x81, 0x00,                     /*   Input (Data, Array, Abs) */
    0xC0,
};

/* ---------------- 状态 ---------------- */
static bool s_inited = false;
static volatile bool s_connected = false;
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint8_t s_own_addr_type = 0;

/* GATT 句柄 */
static uint16_t h_consumer_report;
static uint16_t h_mouse_report;

/* ---------------- UUID ---------------- */
static const ble_uuid16_t uuid_svc_hid   = BLE_UUID16_INIT(0x1812);
static const ble_uuid16_t uuid_svc_bat   = BLE_UUID16_INIT(0x180F);
static const ble_uuid16_t uuid_chr_hid_info = BLE_UUID16_INIT(0x2A4A);
static const ble_uuid16_t uuid_chr_report_map = BLE_UUID16_INIT(0x2A4B);
static const ble_uuid16_t uuid_chr_proto_mode = BLE_UUID16_INIT(0x2A4E);
static const ble_uuid16_t uuid_chr_report = BLE_UUID16_INIT(0x2A4D);
static const ble_uuid16_t uuid_dsc_report_ref = BLE_UUID16_INIT(0x2908);
static const ble_uuid16_t uuid_chr_battery = BLE_UUID16_INIT(0x2A19);
static const ble_uuid16_t uuid_svc_dis = BLE_UUID16_INIT(0x180A);
static const ble_uuid16_t uuid_chr_pnp = BLE_UUID16_INIT(0x2A50);

/* Report Reference 描述符值: {Report ID, 类型(1=Input)} */
static const uint8_t report_ref_consumer[2] = { 0x01, 0x01 };
static const uint8_t report_ref_mouse[2]    = { 0x02, 0x01 };
/* HID Information: bcdHID=1.1, country=0, flags=normally connectable */
static const uint8_t hid_info_val[4] = { 0x01, 0x01, 0x00, 0x02 };
static uint8_t proto_mode_val = 0x01;         /* 当前协议: 1=report, 0=boot(主机可写切换) */static const uint8_t battery_val = 100;
/* PnP ID: Win/BLE HID 类驱动强制要求, 缺失则配对成功也不创建 HID 设备
 * {来源=USB-IF, VID=0x303A(Espressif), PID=0x4004, 版本=1.0} */
static const uint8_t pnp_id_val[7] = { 0x02, 0x3A, 0x30, 0x04, 0x40, 0x00, 0x01 };

static void adv_start(void);

/* ---------------- GATT 访问回调 ---------------- */

static int hid_info_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, hid_info_val, sizeof(hid_info_val));
}

static int report_map_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, s_report_map, sizeof(s_report_map));
}

static int proto_mode_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return os_mbuf_append(ctxt->om, &proto_mode_val, 1);
    }
    /* 写入 = 主机切换协议: 0=boot, 1=report。
     * boot 模式下 mouse 报文必须是 3 字节裸格式(无 Report ID),
     * consumer 无 boot 格式(静默不发), 否则主机按 boot 格式解析 report 报文 = 乱点 */
    uint16_t wlen = OS_MBUF_PKTLEN(ctxt->om);
    uint8_t mode = proto_mode_val;
    if (wlen == 1) {
        mode = ctxt->om->om_data[0];
    }
    if (mode != proto_mode_val) {
        ESP_LOGW(TAG, "protocol mode switch: %u -> %u (%s)", proto_mode_val, mode,
                 mode == 0 ? "BOOT" : "report");
        proto_mode_val = mode;
    }
    return 0;
}

static int report_read_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* 输入报告读取返回全 0 (无按键/无移动); 长度与报告描述符一致 */
    uint8_t len = (uint8_t)(uintptr_t)arg;
    static const uint8_t zero5[5] = {0};
    return os_mbuf_append(ctxt->om, zero5, len);
}

static int report_ref_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const uint8_t *ref = (const uint8_t *)arg;
    return os_mbuf_append(ctxt->om, ref, 2);
}

static int pnp_id_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, pnp_id_val, sizeof(pnp_id_val));
}

static int battery_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    return os_mbuf_append(ctxt->om, &battery_val, 1);
}

/* ---------------- GATT 服务表 ---------------- */

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {   /* HID Service */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc_hid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {   /* HID Information */
                .uuid = &uuid_chr_hid_info.u,
                .access_cb = hid_info_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {   /* Report Map */
                .uuid = &uuid_chr_report_map.u,
                .access_cb = report_map_cb,
                .flags = BLE_GATT_CHR_F_READ,
            },
            {   /* Protocol Mode */
                .uuid = &uuid_chr_proto_mode.u,
                .access_cb = proto_mode_cb,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {   /* Input Report: Consumer Control */
                .uuid = &uuid_chr_report.u,
                .access_cb = report_read_cb,
                .arg = (void *)(uintptr_t)3,   /* {id, usage_lo, usage_hi} */
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &h_consumer_report,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = &uuid_dsc_report_ref.u,
                      .att_flags = BLE_ATT_F_READ,
                      .access_cb = report_ref_cb,
                      .arg = (void *)report_ref_consumer },
                    { 0 },
                },
            },
            {   /* Input Report: Mouse */
                .uuid = &uuid_chr_report.u,
                .access_cb = report_read_cb,
                .arg = (void *)(uintptr_t)5,   /* {id, buttons, dx, dy, wheel} */
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &h_mouse_report,
                .descriptors = (struct ble_gatt_dsc_def[]) {
                    { .uuid = &uuid_dsc_report_ref.u,
                      .att_flags = BLE_ATT_F_READ,
                      .access_cb = report_ref_cb,
                      .arg = (void *)report_ref_mouse },
                    { 0 },
                },
            },
            { 0 },
        },
    },
    {   /* Battery Service */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc_bat.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &uuid_chr_battery.u,
              .access_cb = battery_cb,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    {   /* Device Information Service: PnP ID 是 Windows BLE HID 实例化的硬性要求 */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &uuid_svc_dis.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &uuid_chr_pnp.u,
              .access_cb = pnp_id_cb,
              .flags = BLE_GATT_CHR_F_READ },
            { 0 },
        },
    },
    { 0 },
};

/* ---------------- GAP ---------------- */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "CONNECT: status=%d handle=%d", event->connect.status,
                 event->connect.conn_handle);
        if (event->connect.status == 0) {
            s_connected = true;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "PC connected (HID ready)");
        } else {
            adv_start();
        }
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        ESP_LOGW(TAG, "DISCONNECT: reason=0x%02X (0x13=远端主动断开, 0x3E=配对/链路建立失败, "
                      "0x16=加密失败, 0x08=超时)", event->disconnect.reason);
        adv_start();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "ENC_CHANGE: status=%d", event->enc_change.status);
        return 0;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        const char *what = "OTHER";
        if (event->subscribe.attr_handle == h_consumer_report + 1) {
            what = "CONSUMER_CCCD";
        } else if (event->subscribe.attr_handle == h_mouse_report + 1) {
            what = "MOUSE_CCCD";
        }
        ESP_LOGI(TAG, "SUBSCRIBE: %s handle=%d cur=%d prev=%d reason=%d",
                 what, event->subscribe.attr_handle, event->subscribe.cur_notify,
                 event->subscribe.prev_notify, event->subscribe.reason);
        return 0;
    }
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU: %d", event->mtu.value);
        return 0;
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* 已持有该设备的旧绑定: 必须先删除旧绑定再继续配对。
         * 只返回 RETRY 而不删除会造成无限 REPEAT_PAIRING 风暴
         * (配对->撞旧绑定->再配对, 几 ms 一次, 刷死 nimble_host 并触发 WDT) */
        struct ble_gap_conn_desc desc;
        ESP_LOGI(TAG, "REPEAT_PAIRING: delete stale bond, retry pairing");
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        return 0;
    default:
        return 0;
    }
}

static void adv_start(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));

    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = HID_APPEARANCE;
    fields.appearance_is_present = 1;
    fields.uuids16 = (ble_uuid16_t[]) { BLE_UUID16_INIT(0x1812) };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed rc=%d", rc);
        return;
    }

    struct ble_gap_adv_params params = { 0 };
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 100-200ms: 快速广播会与 WiFi 抢空口(coex), 实测加剧入站丢包 */
    params.itvl_min = 160;
    params.itvl_max = 320;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv start failed rc=%d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure addr failed rc=%d", rc);
        return;
    }
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    /* 句柄注册号: 用于核对 notify/SUBSCRIBE 日志里 att_handle 的归属 */
    ESP_LOGI(TAG, "report handles: consumer=%u (cccd=%u) mouse=%u (cccd=%u)",
             h_consumer_report, h_consumer_report + 1,
             h_mouse_report, h_mouse_report + 1);
    adv_start();
    ESP_LOGI(TAG, "BLE HID ready, pairing name: %s", HID_DEV_NAME);
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason=%d", reason);
}

static void host_task(void *param)
{
    nimble_port_run();              /* 阻塞至 nimble_port_stop */
    nimble_port_freertos_deinit();
}

/* ---------------- 报告发送 ---------------- */

static void notify(uint16_t handle, const uint8_t *data, size_t len)
{
    if (!s_connected || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, handle, om);
    if (rc != 0) {
        /* 限频诊断: rc=3(BLE_HS_ENOTCONN)/6(EBUSY) 等说明链路或订阅状态异常 */
        static uint32_t last_fail_log = 0;
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if (now - last_fail_log > 1000) {
            last_fail_log = now;
            ESP_LOGW(TAG, "notify failed handle=%u len=%u rc=%d", handle, (unsigned)len, rc);
        }
    }
}

/* ---------------- 公共 API ---------------- */

bool blehid_is_connected(void)
{
    return s_connected;
}

void blehid_disconnect(void)
{
    if (s_connected && s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void blehid_unpair_all(void)
{
    /* 先断开当前连接(否则加密链路悬在旧密钥上), 再逐个删除绑定 */
    blehid_disconnect();
    while (ble_gap_unpair_oldest_peer() == 0) {
    }
    ESP_LOGI(TAG, "all bonds cleared");
}

void blehid_consumer_send(uint16_t usage)
{
    if (!s_connected || proto_mode_val == 0) {
        return;   /* boot 协议无 consumer 格式 */
    }
    uint8_t press[3] = { 0x01, (uint8_t)(usage & 0xFF), (uint8_t)(usage >> 8) };
    uint8_t release[3] = { 0x01, 0x00, 0x00 };
    notify(h_consumer_report, press, 3);
    vTaskDelay(pdMS_TO_TICKS(8));
    notify(h_consumer_report, release, 3);
}

void blehid_mouse_scroll(int8_t wheel)
{
    if (proto_mode_val == 0) {
        /* boot 协议 3 字节鼠标无滚轮字段, 只能发 0 移动保持连接活性无意义, 跳过 */
        return;
    }
    uint8_t report[5] = { 0x02, 0x00, 0x00, 0x00, (uint8_t)wheel };
    notify(h_mouse_report, report, 5);
}

void blehid_mouse_move(int8_t dx, int8_t dy)
{
    if (proto_mode_val == 0) {
        uint8_t boot[3] = { 0x00, (uint8_t)dx, (uint8_t)dy };   /* boot: {buttons,dx,dy} */
        notify(h_mouse_report, boot, 3);
        return;
    }
    uint8_t report[5] = { 0x02, 0x00, (uint8_t)dx, (uint8_t)dy, 0x00 };
    notify(h_mouse_report, report, 5);
}

esp_err_t blehid_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble port init failed");

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    /* Just Works 配对 + 绑定(NVS 持久化, 断线重连免配对) */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_device_name_set(HID_DEV_NAME);
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    rc |= ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts add svcs failed rc=%d", rc);
        return ESP_FAIL;
    }

    /* 绑定信息持久化: CONFIG_BT_NIMBLE_NVS_PERSIST=y 时由 IDF 自动注册 */

    nimble_port_freertos_init(host_task);
    s_inited = true;
    ESP_LOGI(TAG, "BLE HID initializing (device: %s)", HID_DEV_NAME);
    return ESP_OK;
}