/*
 * BLE Nordic UART Service (NUS) — ESP32-S3 内置射频，无需额外硬件接线。
 * Service: 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 * RX (PC→ESP write): 6E400002-...
 * TX (ESP→PC notify): 6E400003-...
 */
#include "ble_nus_host.h"
#include "host_transport.h"

#include <assert.h>
#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_nus";

#define DEVICE_NAME "QTgame-Joy"

static ble_nus_rx_byte_fn s_rx_cb;
static uint16_t s_tx_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static bool s_notify_enabled;
static bool s_adv_active;

static const ble_uuid128_t s_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_chr_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_chr_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static int gatt_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg);

/* 特征表须为静态存储；勿在 on_sync 里调用 ble_gatts_start（IDF 示例仅 count_cfg + add_svcs） */
static const struct ble_gatt_chr_def s_nus_chr_defs[] = {
    {
        .uuid = &s_chr_rx_uuid.u,
        .access_cb = gatt_chr_access,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
    },
    {
        .uuid = &s_chr_tx_uuid.u,
        .access_cb = gatt_chr_access,
        .val_handle = &s_tx_val_handle,
        .flags = BLE_GATT_CHR_F_NOTIFY,
    },
    { 0 },
};

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = s_nus_chr_defs,
    },
    { 0 },
};

static void gatt_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    (void)arg;
    if (ctxt->op == BLE_GATT_REGISTER_OP_CHR
        && ble_uuid_cmp(ctxt->chr.chr_def->uuid, &s_chr_tx_uuid.u) == 0) {
        s_tx_val_handle = ctxt->chr.val_handle;
    }
}

static int gatt_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t om_len = OS_MBUF_PKTLEN(ctxt->om);
        for (uint16_t i = 0; i < om_len; ++i) {
            uint8_t b;
            if (os_mbuf_copydata(ctxt->om, i, 1, &b) == 0 && s_rx_cb) {
                s_rx_cb(b);
            }
        }
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            if (host_transport_active_link() == HOST_LINK_UART) {
                ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                break;
            }
            s_conn_handle = event->connect.conn_handle;
            s_adv_active = false;
            host_transport_on_ble_connected();
            ESP_LOGI(TAG, "BLE connected handle=%u", s_conn_handle);
        } else {
            ble_nus_host_advertise();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        s_notify_enabled = false;
        host_transport_on_ble_disconnected();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_notify_enabled = event->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_active = false;
        break;
    default:
        break;
    }
    return 0;
}

static void start_advertising(void)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE || s_adv_active) {
        return;
    }

    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)DEVICE_NAME;
    fields.name_len = strlen(DEVICE_NAME);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv_set_fields %d", rc);
        return;
    }

    struct ble_gap_adv_params adv_params;
    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &adv_params, gap_event, NULL);
    if (rc == 0) {
        s_adv_active = true;
        ESP_LOGI(TAG, "BLE advertising as \"%s\"", DEVICE_NAME);
    } else {
        ESP_LOGW(TAG, "adv_start %d", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr %d", rc);
        return;
    }
    start_advertising();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "reset reason=%d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_nus_host_set_rx_callback(ble_nus_rx_byte_fn cb)
{
    s_rx_cb = cb;
}

void ble_nus_host_init(void)
{
    int rc;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(nimble_port_init());

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.gatts_register_cb = gatt_register_cb;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 0;

    ble_svc_gap_device_name_set(DEVICE_NAME);
    ble_svc_gap_init();
    ble_svc_gatt_init();
    rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(s_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs %d", rc);
        return;
    }

    nimble_port_freertos_init(host_task);
}

void ble_nus_host_advertise(void)
{
    if (ble_hs_synced()) {
        start_advertising();
    }
}

void ble_nus_host_stop_advertise(void)
{
    if (s_adv_active) {
        ble_gap_adv_stop();
        s_adv_active = false;
    }
}

bool ble_nus_host_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

int ble_nus_host_send(const uint8_t *data, size_t len)
{
    if (!ble_nus_host_connected() || !s_notify_enabled || len == 0) {
        return -1;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return -1;
    }
    int rc = ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    return (rc == 0) ? (int)len : -1;
}
