/*
 * ble_service.c - NimBLE GATT server for the FallSena wearable (#FW5).
 *
 * See ble_service.h for the public API, the GATT layout, the UUIDs and the
 * on-air payload formats the phone app implements against.
 *
 * Design notes:
 *   - The NimBLE host runs in its own FreeRTOS task (nimble_port_freertos).
 *     ble_service_init() starts it and returns; advertising begins once the
 *     host signals "sync".
 *   - The characteristic values live in the small static state variables
 *     below (s_batt_pct, s_charging, s_fall_state). GATT reads pull from
 *     them, and notifications are triggered with ble_gatts_chr_updated(),
 *     which re-reads the value via the access callback -- one source of
 *     truth. Those variables are written from the 50 Hz main-loop task and
 *     read from the host task; they are single-byte-ish scalars, so the race
 *     is benign for status reporting.
 *   - Every public call is non-blocking: ble_gatts_chr_updated() just queues
 *     notifications for subscribed peers and returns; it is a no-op when
 *     nobody is subscribed / nothing is connected.
 */

#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "ble_service.h"

static const char *TAG = "ble_svc";

#define BLE_DEVICE_NAME "FallSena"

/* Standard Battery Service (0x180F) / Battery Level (0x2A19). */
#define GATT_UUID_BATTERY_SVC   0x180F
#define GATT_UUID_BATTERY_LEVEL 0x2A19

/*
 * Custom FallSena service + characteristics. Canonical (big-endian) forms,
 * documented for the app team in ble_service.h:
 *   Service      : fa110000-5e5a-4fa1-1e00-000000000000
 *   Fall-Event   : fa110001-5e5a-4fa1-1e00-000000000000
 *   Device-Status: fa110002-5e5a-4fa1-1e00-000000000000
 *   Command      : fa110003-5e5a-4fa1-1e00-000000000000  (write, app -> device)
 * BLE_UUID128_INIT() takes the 16 bytes little-endian (reverse of the string).
 */
static const ble_uuid128_t fallsena_svc_uuid =
    BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e,
                     0xa1, 0x4f, 0x5a, 0x5e, 0x00, 0x00, 0x11, 0xfa);
static const ble_uuid128_t fallsena_fall_evt_uuid =
    BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e,
                     0xa1, 0x4f, 0x5a, 0x5e, 0x01, 0x00, 0x11, 0xfa);
static const ble_uuid128_t fallsena_status_uuid =
    BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e,
                     0xa1, 0x4f, 0x5a, 0x5e, 0x02, 0x00, 0x11, 0xfa);
static const ble_uuid128_t fallsena_cmd_uuid =
    BLE_UUID128_INIT(0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1e,
                     0xa1, 0x4f, 0x5a, 0x5e, 0x03, 0x00, 0x11, 0xfa);

/* Command char: 1-byte opcodes the app writes to the device. */
#define FALLSENA_CMD_CANCEL 0x01  /* cancel the active fall grace countdown */

/* Current characteristic values (see design notes). */
static uint8_t s_batt_pct;       /* 0..100                         */
static uint8_t s_charging;       /* 0 = not charging/unknown, 1 = charging */
static uint8_t s_fall_state;     /* 0 = none, 1 = confirmed, 2 = suspected */

/* App-requested cancel flag: set (write-only, from the host task) when the app
 * writes FALLSENA_CMD_CANCEL to the Command char; read-and-cleared by the main
 * loop via ble_service_poll_app_cancel(). */
static volatile uint8_t s_app_cancel_requested;

/* Value handles, filled in during GATT registration; used for notifications. */
static uint16_t s_batt_level_handle;
static uint16_t s_fall_evt_handle;
static uint16_t s_status_handle;
static uint16_t s_cmd_handle;

/* Address type chosen at sync time for advertising. */
static uint8_t s_own_addr_type;

static void ble_advertise(void);
static int  ble_gap_event(struct ble_gap_event *event, void *arg);

/*
 * Single access callback for every characteristic. The status/fall/battery
 * characteristics are read/notify; the Command characteristic is write-only.
 */
static int
ble_chr_access(uint16_t conn_handle, uint16_t attr_handle,
               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int rc;

    /* Write path: only the write-only Command characteristic accepts writes. */
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        if (attr_handle == s_cmd_handle) {
            uint8_t cmd;
            uint16_t len = 0;
            rc = ble_hs_mbuf_to_flat(ctxt->om, &cmd, sizeof(cmd), &len);
            if (rc != 0) {
                return BLE_ATT_ERR_UNLIKELY;
            }
            if (len >= 1 && cmd == FALLSENA_CMD_CANCEL) {
                /* Latch the app-cancel request; the main loop polls and clears
                 * it via ble_service_poll_app_cancel(). */
                s_app_cancel_requested = 1;
            }
            return 0;
        }
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (attr_handle == s_batt_level_handle) {
        uint8_t v = s_batt_pct;
        rc = os_mbuf_append(ctxt->om, &v, sizeof(v));
    } else if (attr_handle == s_fall_evt_handle) {
        uint8_t v = s_fall_state;
        rc = os_mbuf_append(ctxt->om, &v, sizeof(v));
    } else if (attr_handle == s_status_handle) {
        uint8_t v[2] = { s_batt_pct, s_charging };
        rc = os_mbuf_append(ctxt->om, v, sizeof(v));
    } else {
        return BLE_ATT_ERR_UNLIKELY;
    }

    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* GATT service / characteristic table. */
static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        /* Standard Battery Service. */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(GATT_UUID_BATTERY_SVC),
        .characteristics = (struct ble_gatt_chr_def[]) { {
            .uuid = BLE_UUID16_DECLARE(GATT_UUID_BATTERY_LEVEL),
            .access_cb = ble_chr_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_batt_level_handle,
        }, {
            0, /* no more characteristics */
        } },
    },
    {
        /* Custom FallSena service. */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &fallsena_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) { {
            /* Fall-Event: read + notify, 1-byte state. */
            .uuid = &fallsena_fall_evt_uuid.u,
            .access_cb = ble_chr_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_fall_evt_handle,
        }, {
            /* Device-Status: read + notify, [pct, charging]. */
            .uuid = &fallsena_status_uuid.u,
            .access_cb = ble_chr_access,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
            .val_handle = &s_status_handle,
        }, {
            /* Command: write-only, 1-byte opcode (app -> device). */
            .uuid = &fallsena_cmd_uuid.u,
            .access_cb = ble_chr_access,
            .flags = BLE_GATT_CHR_F_WRITE,
            .val_handle = &s_cmd_handle,
        }, {
            0, /* no more characteristics */
        } },
    },
    {
        0, /* no more services */
    },
};

/* Start (or restart) connectable, general-discoverable advertising. */
static void
ble_advertise(void)
{
    struct ble_gap_adv_params adv_params;
    struct ble_hs_adv_fields fields;
    const char *name;
    int rc;

    memset(&fields, 0, sizeof(fields));

    /* General discoverable, BR/EDR (classic BT) not supported. */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    /* Let the stack fill the TX power level automatically. */
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    /* Advertise the standard Battery Service UUID so the app can filter. */
    fields.uuids16 = (ble_uuid16_t[]) {
        BLE_UUID16_INIT(GATT_UUID_BATTERY_SVC),
    };
    fields.num_uuids16 = 1;
    fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed; rc=%d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND; /* connectable */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN; /* discoverable */
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_start failed; rc=%d", rc);
        return;
    }
    ESP_LOGI(TAG, "advertising as \"%s\"", name);
}

/* GAP event handler: keep advertising alive across (re)connections. */
static int
ble_gap_event(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);
        if (event->connect.status != 0) {
            /* Failed; resume advertising. */
            ble_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnect; reason=%d", event->disconnect.reason);
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertise complete; reason=%d",
                 event->adv_complete.reason);
        ble_advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "subscribe; conn=%d attr=%d cur_notify=%d",
                 event->subscribe.conn_handle,
                 event->subscribe.attr_handle,
                 event->subscribe.cur_notify);
        return 0;

    default:
        return 0;
    }
}

static void
ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset; reason=%d", reason);
}

static void
ble_on_sync(void)
{
    int rc;

    /* Make sure we have a usable identity address, then pick the adv type. */
    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "ensure_addr failed; rc=%d", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "infer addr type failed; rc=%d", rc);
        return;
    }

    ble_advertise();
}

static void
ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    /* Returns only when nimble_port_stop() is called. */
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static int
ble_gatt_server_init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        return rc;
    }
    return ble_gatts_add_svcs(gatt_svcs);
}

esp_err_t ble_service_init(void)
{
    esp_err_t ret;
    int rc;

    /* NVS is used by the BLE stack (PHY calibration). Safe if already init. */
    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Host configuration callbacks. */
    ble_hs_cfg.reset_cb = ble_on_reset;
    ble_hs_cfg.sync_cb = ble_on_sync;

    rc = ble_gatt_server_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT server init failed; rc=%d", rc);
        return ESP_FAIL;
    }

    rc = ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "set device name failed; rc=%d", rc);
        return ESP_FAIL;
    }

    /* Launch the NimBLE host task; advertising starts on sync. */
    nimble_port_freertos_init(ble_host_task);

    ESP_LOGI(TAG, "BLE service initialized");
    return ESP_OK;
}

void ble_service_notify_fall_confirmed(void)
{
    s_fall_state = 1;
    /* No-op if the handle is not yet registered or nobody is subscribed. */
    if (s_fall_evt_handle != 0) {
        ble_gatts_chr_updated(s_fall_evt_handle);
    }
}

void ble_service_notify_fall_suspected(void)
{
    s_fall_state = 2;
    /* No-op if the handle is not yet registered or nobody is subscribed. */
    if (s_fall_evt_handle != 0) {
        ble_gatts_chr_updated(s_fall_evt_handle);
    }
}

void ble_service_notify_fall_cleared(void)
{
    s_fall_state = 0;
    /* No-op if the handle is not yet registered or nobody is subscribed. */
    if (s_fall_evt_handle != 0) {
        ble_gatts_chr_updated(s_fall_evt_handle);
    }
}

bool ble_service_poll_app_cancel(void)
{
    /* Read-and-clear: report a pending app cancel exactly once. Written from
     * the NimBLE host task, polled from the 50 Hz main loop; the single-byte
     * read-then-clear race is benign for a one-shot cancel request. */
    if (s_app_cancel_requested) {
        s_app_cancel_requested = 0;
        return true;
    }
    return false;
}

void ble_service_update_status(int battery_pct, bool charging)
{
    if (battery_pct < 0) {
        battery_pct = 0;
    } else if (battery_pct > 100) {
        battery_pct = 100;
    }
    s_batt_pct = (uint8_t)battery_pct;
    s_charging = charging ? 1 : 0;

    /* Push both the standard Battery Level and the custom status char. Each
     * call is a no-op if the handle is unregistered or nobody is subscribed. */
    if (s_batt_level_handle != 0) {
        ble_gatts_chr_updated(s_batt_level_handle);
    }
    if (s_status_handle != 0) {
        ble_gatts_chr_updated(s_status_handle);
    }
}
