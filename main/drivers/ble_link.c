/**
 * Peripheral: advertise as ESP32Monitor and expose a Nordic UART style
 * service. Writes are the PC's lines. Notifies are the replies.
 *
 * Windows Just Works pairing cannot be undone from this side. A new computer
 * has to type the 6-digit code shown here. A later connection from that bond
 * encrypts without another code.
 */

#include "ble_link.h"
#include "usb_serial_comm.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_att.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "store/config/ble_store_config.h"
#include "lvgl.h"

static const char *TAG = "BLE";

#define BLE_NAME            "ESP32Monitor"
/* Generic Display. Windows picks the Settings icon from this value. */
#define BLE_APPEARANCE      0x0140
#define BLE_TOKEN_PATH      "/storage/ble_token.txt"
#define BLE_TOKEN_LEN       8
#define BLE_CONFIRM_MS       30000

/* 6E400001-B5A3-F393-E0A9-E50E24DCCA9E and the two characteristics.
 * NimBLE stores a 128-bit UUID little-endian. */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static uint16_t s_rx_val;
static uint16_t s_tx_val;
static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static struct ble_npl_callout s_sec_callout;
static bool s_sec_callout_ready;
static bool s_sec_asked;
static bool s_subscribed;
static bool s_authorized;
static bool s_send_ok;
static volatile int s_ui_want;
static char s_token[BLE_TOKEN_LEN + 1];
static StreamBufferHandle_t s_rx_stream;
static lv_obj_t *s_overlay;
static lv_obj_t *s_btn;
static lv_obj_t *s_ok;
static uint32_t s_prompt_until;
static volatile int s_show_code;
static volatile uint32_t s_code;
static char s_code_drawn[8];

static int gap_event(struct ble_gap_event *event, void *arg);
void ble_store_config_init(void);
static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &s_rx_val,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = gatt_access,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_val,
            },
            { 0 }
        },
    },
    { 0 }
};

static void token_load(void)
{
    s_token[0] = 0;
    FILE *f = fopen(BLE_TOKEN_PATH, "r");
    if (!f) return;
    char buf[16];
    if (!fgets(buf, sizeof(buf), f)) {
        fclose(f);
        return;
    }
    fclose(f);
    size_t n = strcspn(buf, "\r\n");
    if (n != BLE_TOKEN_LEN) return;
    for (size_t i = 0; i < n; i++) {
        char c = buf[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return;
    }
    memcpy(s_token, buf, BLE_TOKEN_LEN);
    s_token[BLE_TOKEN_LEN] = 0;
}

static void token_save(const char *token)
{
    memcpy(s_token, token, BLE_TOKEN_LEN);
    s_token[BLE_TOKEN_LEN] = 0;
    FILE *f = fopen(BLE_TOKEN_PATH, "w");
    if (!f) return;
    fputs(s_token, f);
    fclose(f);
}

static void notify_bytes(const char *data, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || !s_subscribed || !data || !len) return;
    uint16_t mtu = ble_att_mtu(s_conn);
    uint16_t chunk = (mtu > 3) ? (uint16_t)(mtu - 3) : 20;
    if (chunk > 240) chunk = 240;
    while (len) {
        uint16_t n = (len > chunk) ? chunk : (uint16_t)len;
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, n);
        if (!om) return;
        if (ble_gatts_notify_custom(s_conn, s_tx_val, om) != 0) return;
        data += n;
        len -= n;
    }
}

static void tx_sink(const char *data, size_t len)
{
    if (!s_authorized) return;
    notify_bytes(data, len);
}

static void authorize(void)
{
    s_authorized = true;
    s_ui_want = 0;
    s_prompt_until = 0;
    s_show_code = 0;
}

static void issue_token(void)
{
    char token[BLE_TOKEN_LEN + 1];
    uint32_t a = esp_random();
    uint32_t b = esp_random();
    snprintf(token, sizeof(token), "%04lx%04lx",
             (unsigned long)(a & 0xffff), (unsigned long)(b & 0xffff));
    token_save(token);
    authorize();
    char line[24];
    int n = snprintf(line, sizeof(line), "BLE_OK:%s\n", s_token);
    if (s_subscribed && n > 0) notify_bytes(line, (size_t)n);
    else s_send_ok = true;
}

static void passkey_begin(uint16_t conn, uint32_t key, uint8_t action)
{
    struct ble_sm_io io;
    memset(&io, 0, sizeof(io));
    io.action = action;
    if (action == BLE_SM_IOACT_NUMCMP) io.numcmp_accept = 1;
    else io.passkey = key;
    if (ble_sm_inject_io(conn, &io) != 0) return;
    s_code = key;
    s_show_code = 1;
    s_code_drawn[0] = 0;
    s_prompt_until = (uint32_t)(esp_timer_get_time() / 1000) + BLE_CONFIRM_MS;
    s_ui_want = 1;
}

static void overlay_create(void)
{
    if (s_overlay) return;
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, 200, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *caption = lv_label_create(s_overlay);
    lv_label_set_text(caption, "Bluetooth");
    lv_obj_set_style_text_font(caption, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(caption, lv_color_hex(0x90CAF9), 0);
    lv_obj_align(caption, LV_ALIGN_CENTER, 0, -142);
    lv_obj_remove_flag(caption, LV_OBJ_FLAG_CLICKABLE);

    s_btn = lv_obj_create(s_overlay);
    lv_obj_set_size(s_btn, 300, 120);
    lv_obj_align(s_btn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_radius(s_btn, 28, 0);
    lv_obj_set_style_bg_color(s_btn, lv_color_hex(0x141418), 0);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_btn, 3, 0);
    lv_obj_set_style_pad_all(s_btn, 0, 0);
    lv_obj_remove_flag(s_btn, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_ok = lv_label_create(s_btn);
    lv_label_set_text(s_ok, "000000");
    lv_obj_set_style_text_font(s_ok, &lv_font_montserrat_42, 0);
    lv_obj_set_style_text_color(s_ok, lv_color_white(), 0);
    lv_obj_center(s_ok);
    lv_obj_remove_flag(s_ok, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *hint = lv_label_create(s_overlay);
    lv_label_set_text(hint, "Type on PC");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_22, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x90CAF9), 0);
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 142);
    lv_obj_remove_flag(hint, LV_OBJ_FLAG_CLICKABLE);
}

bool ble_link_wants_ui(void)
{
    return s_ui_want != 0;
}

static void overlay_show(void)
{
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
}

void ble_link_ui_poll(uint32_t now_ms)
{
    overlay_create();
    if (!s_overlay) return;
    if (s_show_code && s_prompt_until && (int32_t)(s_prompt_until - now_ms) > 0) {
        char text[8];
        snprintf(text, sizeof(text), "%06lu", (unsigned long)s_code);
        if (strcmp(s_code_drawn, text) != 0) {
            memcpy(s_code_drawn, text, sizeof(text));
            lv_label_set_text(s_ok, text);
        }
        s_ui_want = 1;
        overlay_show();
        return;
    }
    if (s_show_code && s_prompt_until) {
        s_show_code = 0;
        s_prompt_until = 0;
        s_ui_want = 0;
        if (s_conn != BLE_HS_CONN_HANDLE_NONE && !s_authorized) {
            ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
    if (!lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    }
}

static void flush_ok(void)
{
    if (!s_send_ok || !s_subscribed || !s_token[0]) return;
    char line[24];
    int n = snprintf(line, sizeof(line), "BLE_OK:%s\n", s_token);
    s_send_ok = false;
    if (n > 0) notify_bytes(line, (size_t)n);
}

static void handle_complete_line(char *line)
{
    flush_ok();
    if (!s_authorized) return;
    usb_serial_handle_line_from(line, true);
}

static void ble_rx_task(void *arg)
{
    (void)arg;
    char acc[1024];
    int n = 0;
    uint8_t b;
    while (1) {
        if (xStreamBufferReceive(s_rx_stream, &b, 1, pdMS_TO_TICKS(500)) != 1) continue;
        if (b == '\r') continue;
        if (b != '\n') {
            if (n < (int)sizeof(acc) - 1) acc[n++] = (char)b;
            else n = (int)sizeof(acc);
            continue;
        }
        if (n > 0 && n < (int)sizeof(acc)) {
            acc[n] = 0;
            handle_complete_line(acc);
        }
        n = 0;
    }
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR || attr_handle != s_rx_val) {
        return 0;
    }
    uint8_t tmp[256];
    uint16_t len = 0;
    int rc = ble_hs_mbuf_to_flat(ctxt->om, tmp, sizeof(tmp), &len);
    if (rc != 0) return BLE_ATT_ERR_UNLIKELY;
    if (len && s_rx_stream) {
        xStreamBufferSend(s_rx_stream, tmp, len, 0);
    }
    return 0;
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.appearance = BLE_APPEARANCE;
    fields.appearance_is_present = 1;
    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "adv fields %d", rc);
        return;
    }
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = &s_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) ESP_LOGW(TAG, "adv rsp %d", rc);

    struct ble_gap_adv_params params;
    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = 160;
    params.itvl_max = 240;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (rc != 0) ESP_LOGW(TAG, "adv start %d", rc);
}

/* Windows will not complete a GATT write on this bond until the link is
 * encrypted, and it does not start encryption by itself. Ask after it has
 * finished the connection update. Asking inside on_connect drops the link. */
static void security_later(struct ble_npl_event *ev)
{
    (void)ev;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || s_authorized) return;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(s_conn, &desc) != 0) return;
    if (desc.sec_state.encrypted) {
        issue_token();
        return;
    }
    ble_gap_security_initiate(s_conn);
}

static void on_connect(uint16_t handle)
{
    s_conn = handle;
    s_subscribed = false;
    s_authorized = false;
    s_send_ok = false;
    s_show_code = 0;
    s_ui_want = 0;
    s_prompt_until = 0;
    s_sec_asked = false;
    struct ble_gap_conn_desc desc;
    if (ble_gap_conn_find(handle, &desc) == 0 && desc.sec_state.encrypted) {
        issue_token();
        return;
    }
    /* Windows does not encrypt a saved bond on its own. Ask once the link
     * has settled. Connection-parameter updates are not guaranteed. */
    if (s_sec_callout_ready && !s_sec_asked) {
        s_sec_asked = true;
        ble_npl_callout_reset(&s_sec_callout, ble_npl_time_ms_to_ticks32(800));
    }
}

static void on_disconnect(void)
{
    if (s_sec_callout_ready) ble_npl_callout_stop(&s_sec_callout);
    s_sec_asked = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_subscribed = false;
    s_authorized = false;
    s_send_ok = false;
    s_show_code = 0;
    s_prompt_until = 0;
    s_ui_want = 0;
    advertise();
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) on_connect(event->connect.conn_handle);
        else advertise();
        return 0;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disc %d", event->disconnect.reason);
        on_disconnect();
        return 0;
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0 &&
            event->enc_change.conn_handle == s_conn) {
            issue_token();
        }
        return 0;
    case BLE_GAP_EVENT_PASSKEY_ACTION: {
        uint8_t action = event->passkey.params.action;
        uint32_t key = esp_random() % 1000000;
        if (action == BLE_SM_IOACT_NUMCMP) key = event->passkey.params.numcmp;
        else action = BLE_SM_IOACT_DISP;
        passkey_begin(event->passkey.conn_handle, key, action);
        return 0;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* Windows still lists the device as paired after it has deleted
         * the link key (BTHUSB event 10). Refusing the new pairing makes
         * the next authentication fail and the link drop. Drop our stale
         * keys once and accept the new passkey. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val) {
            s_subscribed = event->subscribe.cur_notify != 0;
            /* The PC enables notify before the bond is encrypted. Hand out
             * the token then, or the first line is ignored. */
            if (s_subscribed && !s_authorized) issue_token();
        }
        return 0;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.status == 0 &&
            event->conn_update.conn_handle == s_conn &&
            !s_sec_asked && s_sec_callout_ready) {
            s_sec_asked = true;
            ble_npl_callout_reset(&s_sec_callout, ble_npl_time_ms_to_ticks32(400));
        }
        return 0;
    case BLE_GAP_EVENT_MTU:
        return 0;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;
    default:
        return 0;
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset %d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) return;
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) return;
    ble_att_set_preferred_mtu(247);
    advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_link_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs %d", err);
        return;
    }

    token_load();
    s_rx_stream = xStreamBufferCreate(2048, 1);
    if (!s_rx_stream) return;
    if (xTaskCreate(ble_rx_task, "ble_rx", 4096, NULL, 2, NULL) != pdPASS) return;

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nimble %d", err);
        return;
    }
    ble_npl_callout_init(&s_sec_callout, nimble_port_get_dflt_eventq(),
                         security_later, NULL);
    s_sec_callout_ready = true;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(BLE_NAME);
    ble_svc_gap_device_appearance_set(BLE_APPEARANCE);
    if (ble_gatts_count_cfg(s_svcs) != 0 || ble_gatts_add_svcs(s_svcs) != 0) {
        ESP_LOGW(TAG, "gatt");
        return;
    }
    ble_store_config_init();
    usb_serial_set_tx_fn(tx_sink);
    nimble_port_freertos_init(host_task);
}
