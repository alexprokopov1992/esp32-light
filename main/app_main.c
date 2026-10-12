#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <sys/param.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "nvs.h"
#include "nvs_flash.h"

#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_http_client.h"

#include "esp_sntp.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"

#include "lwip/inet.h"
#include "lwip/ip_addr.h"
#include "lwip/err.h"

#include "esp_ping.h"
#include "driver/gpio.h"

#include "esp_timer.h"

#include "ping/ping_sock.h"

#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "lcd_st7789.h"
#include "display_ui.h"

static const char *TAG = "power_pinger";

/* ---------- НАСТРОЙКИ ---------- */
#define BTN_GPIO                GPIO_NUM_0     // BOOT кнопка
#define BTN_HOLD_MS             800            // удержание для входа в конфиг
#define AP_SSID                 "PowerPinger-Setup"
#define AP_PASS                 "12345678"     // минимум 8
#define AP_MAX_CONN             4

#define WIFI_MAX_RETRIES        8
#define PING_INTERVAL_MS        1000

// ✅ OFF тільки після N мс без жодного успішного ping
// Рекомендовано 60000..120000, щоб не флапало від коротких провалів
#define OFF_CONFIRM_MS          (2 * 60 * 1000)   // 2 minutes without a successful ping

#define ON_CONFIRM_STREAK       3              // скільки success підряд потрібно для ON

#define NVS_NS                  "cfg"
#define KEY_SSID                "ssid"
#define KEY_PASS                "pass"
#define KEY_TG_TOKEN            "tg_token"
#define KEY_TG_CHAT             "tg_chat"
#define KEY_PING_IP             "ping_ip"
#define KEY_PING_IP2            "ping_ip2"
#define KEY_PING_IP3            "ping_ip3"
#define KEY_LAST_STATUS         "last_stat"    // u8: 1=on, 0=off, 0xFF=unknown
#define KEY_LAST_OFF_TIME       "last_off"     // i64 epoch
#define KEY_LAST_ON_TIME        "last_on"      // i64 epoch

/* ---------- WiFi events ---------- */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

/* ---------- runtime state ---------- */
typedef struct {
    char ssid[33];
    char pass[65];
    char tg_token[128];
    char tg_chat[64];
    char ping_ip[16];  // primary, required
    char ping_ip2[16]; // optional
    char ping_ip3[16]; // optional
    bool valid;
} app_cfg_t;

static app_cfg_t g_cfg;

static httpd_handle_t g_httpd = NULL;

#define MAX_PING_TARGETS 3

typedef struct {
    esp_ping_handle_t handle;
    const char *ip;
    bool enabled;
    volatile bool last_result;
    volatile int64_t last_ok_ms;
    volatile uint32_t ok_streak;
} ping_target_t;

static ping_target_t g_ping_targets[MAX_PING_TARGETS];
static volatile bool last_ping_rez = false; // true if at least one enabled target's latest result is success
static volatile int64_t g_last_ping_ok_ms = 0; // last successful reply from ANY target
static volatile int64_t g_ping_monitor_started_ms = 0;

static volatile bool inited = false;
/* ---------- helpers ---------- */
static int64_t now_ms(void) {
    return (int64_t)(esp_timer_get_time() / 1000);
}

static int64_t now_epoch_s(void) {
    time_t t;
    time(&t);
    return (int64_t)t;
}

static void set_tz_kyiv(void) {
    // setenv("TZ", "Europe/Kyiv", 1);
    setenv("TZ", "EET-2EEST,M3.5.0/3,M10.5.0/4", 1);
    tzset();
}

static void reboot_now(void) {
    ESP_LOGW(TAG, "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* ---------- NVS ---------- */
static const char *nvs_type_name(nvs_type_t type) {
    switch (type) {
        case NVS_TYPE_U8:   return "u8";
        case NVS_TYPE_I8:   return "i8";
        case NVS_TYPE_U16:  return "u16";
        case NVS_TYPE_I16:  return "i16";
        case NVS_TYPE_U32:  return "u32";
        case NVS_TYPE_I32:  return "i32";
        case NVS_TYPE_U64:  return "u64";
        case NVS_TYPE_I64:  return "i64";
        case NVS_TYPE_STR:  return "str";
        case NVS_TYPE_BLOB: return "blob";
        default:            return "unknown";
    }
}

static void nvs_dump_blob_hex(const char *ns, const char *key, nvs_handle_t h) {
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, key, NULL, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS [%s] %s: blob read failed: %s", ns, key, esp_err_to_name(err));
        return;
    }

    if (len == 0) {
        ESP_LOGI(TAG, "NVS [%s] %s (blob, 0 bytes) = <empty>", ns, key);
        return;
    }

    uint8_t *data = malloc(len);
    if (!data) {
        ESP_LOGW(TAG, "NVS [%s] %s: no memory for %u-byte blob", ns, key, (unsigned)len);
        return;
    }

    err = nvs_get_blob(h, key, data, &len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS [%s] %s: blob read failed: %s", ns, key, esp_err_to_name(err));
        free(data);
        return;
    }

    ESP_LOGI(TAG, "NVS [%s] %s (blob, %u bytes):", ns, key, (unsigned)len);
    for (size_t off = 0; off < len; off += 32) {
        char line[32 * 3 + 1];
        size_t chunk = MIN((size_t)32, len - off);
        char *w = line;
        for (size_t i = 0; i < chunk; i++) {
            int n = snprintf(w, (size_t)(line + sizeof(line) - w), "%02X%s",
                             data[off + i], (i + 1 < chunk) ? " " : "");
            if (n < 0) break;
            w += n;
        }
        *w = '\0';
        ESP_LOGI(TAG, "  %04u: %s", (unsigned)off, line);
    }
    free(data);
}

static void dump_all_nvs_entries(void) {
    ESP_LOGI(TAG, "========== NVS DUMP BEGIN ==========");

    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find("nvs", NULL, NVS_TYPE_ANY, &it);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS is empty");
        ESP_LOGI(TAG, "=========== NVS DUMP END ===========");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_entry_find failed: %s", esp_err_to_name(err));
        ESP_LOGI(TAG, "=========== NVS DUMP END ===========");
        return;
    }

    size_t count = 0;
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        count++;

        nvs_handle_t h;
        esp_err_t open_err = nvs_open_from_partition("nvs", info.namespace_name, NVS_READONLY, &h);
        if (open_err != ESP_OK) {
            ESP_LOGW(TAG, "NVS [%s] %s (%s): open failed: %s",
                     info.namespace_name, info.key, nvs_type_name(info.type), esp_err_to_name(open_err));
            err = nvs_entry_next(&it);
            continue;
        }

        switch (info.type) {
            case NVS_TYPE_U8: {
                uint8_t v = 0; esp_err_t e = nvs_get_u8(h, info.key, &v);
                if (e == ESP_OK) ESP_LOGI(TAG, "NVS [%s] %s (u8) = %u", info.namespace_name, info.key, (unsigned)v);
                else ESP_LOGW(TAG, "NVS [%s] %s (u8): %s", info.namespace_name, info.key, esp_err_to_name(e));
                break;
            }
            case NVS_TYPE_I8: {
                int8_t v = 0; esp_err_t e = nvs_get_i8(h, info.key, &v);
                if (e == ESP_OK) ESP_LOGI(TAG, "NVS [%s] %s (i8) = %d", info.namespace_name, info.key, (int)v);
                else ESP_LOGW(TAG, "NVS [%s] %s (i8): %s", info.namespace_name, info.key, esp_err_to_name(e));
                break;
            }
            case NVS_TYPE_U16: {
                uint16_t v = 0; esp_err_t e = nvs_get_u16(h, info.key, &v);
                if (e == ESP_OK) ESP_LOGI(TAG, "NVS [%s] %s (u16) = %u", info.namespace_name, info.key, (unsigned)v);
                else ESP_LOGW(TAG, "NVS [%s] %s (u16): %s", info.namespace_name, info.key, esp_err_to_name(e));
                break;
            }
            case NVS_TYPE_I16: {
                int16_t v = 0; esp_err_t e = nvs_get_i16(h, info.key, &v);
                if (e == ESP_OK) ESP_LOGI(TAG, "NVS [%s] %s (i16) = %d", info.namespace_name, info.key, (int)v);
                else ESP_LOGW(TAG, "NVS [%s] %s (i16): %s", info.namespace_name, info.key, esp_err_to_name(e));
                break;
            }
            case NVS_TYPE_U32: {
                uint32_t v = 0; esp_err_t e = nvs_get_u32(h, info.key, &v);
                if (e == ESP_OK) ESP_LOGI(TAG, "NVS [%s] %s (u32) = %" PRIu32, info.namespace_name, info.key, v);
                else ESP_LOGW(TAG, "NVS [%s] %s (u32): %s", info.namespace_name, info.key, esp_err_to_name(e));
                break;
            }
            case NVS_TYPE_I32: {
                int32_t v = 0; esp_err_t e = nvs_get_i32(h, info.key, &v);
                if (e == ESP_OK) ESP_LOGI(TAG, "NVS [%s] %s (i32) = %" PRId32, info.namespace_name, info.key, v);
                else ESP_LOGW(TAG, "NVS [%s] %s (i32): %s", info.namespace_name, info.key, esp_err_to_name(e));
                break;
            }
            case NVS_TYPE_U64: {
                uint64_t v = 0; esp_err_t e = nvs_get_u64(h, info.key, &v);
                if (e == ESP_OK) ESP_LOGI(TAG, "NVS [%s] %s (u64) = %" PRIu64, info.namespace_name, info.key, v);
                else ESP_LOGW(TAG, "NVS [%s] %s (u64): %s", info.namespace_name, info.key, esp_err_to_name(e));
                break;
            }
            case NVS_TYPE_I64: {
                int64_t v = 0; esp_err_t e = nvs_get_i64(h, info.key, &v);
                if (e == ESP_OK) ESP_LOGI(TAG, "NVS [%s] %s (i64) = %" PRId64, info.namespace_name, info.key, v);
                else ESP_LOGW(TAG, "NVS [%s] %s (i64): %s", info.namespace_name, info.key, esp_err_to_name(e));
                break;
            }
            case NVS_TYPE_STR: {
                size_t len = 0;
                esp_err_t e = nvs_get_str(h, info.key, NULL, &len);
                if (e == ESP_OK && len > 0) {
                    char *v = malloc(len);
                    if (v) {
                        e = nvs_get_str(h, info.key, v, &len);
                        if (e == ESP_OK) ESP_LOGI(TAG, "NVS [%s] %s (str) = \"%s\"", info.namespace_name, info.key, v);
                        else ESP_LOGW(TAG, "NVS [%s] %s (str): %s", info.namespace_name, info.key, esp_err_to_name(e));
                        free(v);
                    } else {
                        ESP_LOGW(TAG, "NVS [%s] %s (str): no memory", info.namespace_name, info.key);
                    }
                } else if (e == ESP_OK) {
                    ESP_LOGI(TAG, "NVS [%s] %s (str) = \"\"", info.namespace_name, info.key);
                } else {
                    ESP_LOGW(TAG, "NVS [%s] %s (str): %s", info.namespace_name, info.key, esp_err_to_name(e));
                }
                break;
            }
            case NVS_TYPE_BLOB:
                nvs_dump_blob_hex(info.namespace_name, info.key, h);
                break;
            default:
                ESP_LOGI(TAG, "NVS [%s] %s (type=0x%02X) = <unsupported type>",
                         info.namespace_name, info.key, (unsigned)info.type);
                break;
        }

        nvs_close(h);
        err = nvs_entry_next(&it);
    }

    if (it != NULL) {
        nvs_release_iterator(it);
    }

    ESP_LOGI(TAG, "NVS entries: %u", (unsigned)count);
    ESP_LOGI(TAG, "=========== NVS DUMP END ===========");
}

static esp_err_t nvs_get_str_safe(nvs_handle_t h, const char *key, char *out, size_t out_sz) {
    size_t required = 0;
    esp_err_t err = nvs_get_str(h, key, NULL, &required);
    if (err != ESP_OK) return err;
    if (required == 0 || required > out_sz) return ESP_ERR_INVALID_SIZE;
    return nvs_get_str(h, key, out, &required);
}

static bool load_cfg_from_nvs(app_cfg_t *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    esp_err_t e1 = nvs_get_str_safe(h, KEY_SSID, cfg->ssid, sizeof(cfg->ssid));
    esp_err_t e2 = nvs_get_str_safe(h, KEY_PASS, cfg->pass, sizeof(cfg->pass));
    esp_err_t e3 = nvs_get_str_safe(h, KEY_TG_TOKEN, cfg->tg_token, sizeof(cfg->tg_token));
    esp_err_t e4 = nvs_get_str_safe(h, KEY_TG_CHAT, cfg->tg_chat, sizeof(cfg->tg_chat));
    esp_err_t e5 = nvs_get_str_safe(h, KEY_PING_IP, cfg->ping_ip, sizeof(cfg->ping_ip));

    // Optional targets: missing keys are valid for devices configured by older firmware.
    (void)nvs_get_str_safe(h, KEY_PING_IP2, cfg->ping_ip2, sizeof(cfg->ping_ip2));
    (void)nvs_get_str_safe(h, KEY_PING_IP3, cfg->ping_ip3, sizeof(cfg->ping_ip3));
    nvs_close(h);

    cfg->valid = (e1 == ESP_OK && e2 == ESP_OK && e3 == ESP_OK && e4 == ESP_OK && e5 == ESP_OK &&
                  strlen(cfg->ssid) > 0 && strlen(cfg->tg_token) > 0 && strlen(cfg->tg_chat) > 0 &&
                  strlen(cfg->ping_ip) > 0);
    return cfg->valid;
}

static bool save_cfg_to_nvs(const app_cfg_t *cfg) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    esp_err_t err = ESP_OK;
    err |= nvs_set_str(h, KEY_SSID, cfg->ssid);
    err |= nvs_set_str(h, KEY_PASS, cfg->pass);
    err |= nvs_set_str(h, KEY_TG_TOKEN, cfg->tg_token);
    err |= nvs_set_str(h, KEY_TG_CHAT, cfg->tg_chat);
    err |= nvs_set_str(h, KEY_PING_IP, cfg->ping_ip);
    err |= nvs_set_str(h, KEY_PING_IP2, cfg->ping_ip2);
    err |= nvs_set_str(h, KEY_PING_IP3, cfg->ping_ip3);
    err |= nvs_commit(h);
    nvs_close(h);
    return (err == ESP_OK);
}

static uint8_t load_last_status(uint8_t def_val) {
    nvs_handle_t h;
    uint8_t v = def_val;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, KEY_LAST_STATUS, &v);
        nvs_close(h);
    }
    return v;
}

static void save_last_status(uint8_t st) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, KEY_LAST_STATUS, st);
        nvs_commit(h);
        nvs_close(h);
    }
}

static int64_t load_i64(const char *key, int64_t def_val) {
    nvs_handle_t h;
    int64_t v = def_val;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i64(h, key, &v);
        nvs_close(h);
    }
    return v;
}

static void save_i64(const char *key, int64_t val) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, key, val);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- Telegram ---------- */
static esp_err_t tg_send_message(const char *token, const char *chat_id, const char *text) {
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);

    char *enc = NULL;
    size_t text_len = strlen(text);
    enc = (char*)calloc(1, text_len * 3 + 1);
    if (!enc) return ESP_ERR_NO_MEM;

    char *p = enc;
    for (size_t i = 0; i < text_len; i++) {
        unsigned char c = (unsigned char)text[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            *p++ = (char)c;
        } else if (c == ' ') {
            *p++ = '+';
        } else if (c == '\n' || c == '\r') {
            *p++ = '%'; *p++ = '0'; *p++ = 'A';
        } else {
            static const char hex[] = "0123456789ABCDEF";
            *p++ = '%';
            *p++ = hex[(c >> 4) & 0xF];
            *p++ = hex[c & 0xF];
        }
    }
    *p = 0;

    char body[512];
    snprintf(body, sizeof(body), "chat_id=%s&text=%s", chat_id, enc);
    free(enc);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        int code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Telegram HTTP %d", code);
        if (code < 200 || code >= 300) err = ESP_FAIL;
    } else {
        ESP_LOGW(TAG, "Telegram send failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

/* ---------- WEB UI (AP + HTTP) ---------- */
static const char *HTML_FORM =
"<!doctype html><html><head><meta charset='utf-8'/>"
"<meta name='viewport' content='width=device-width,initial-scale=1'/>"
"<title>PowerPinger Setup</title>"
"<style>body{font-family:sans-serif;margin:20px;max-width:720px}"
"label{display:block;margin-top:12px}input{width:100%;padding:10px;font-size:16px}"
"button{margin-top:18px;padding:12px 18px;font-size:16px}</style>"
"</head><body>"
"<h2>PowerPinger Setup</h2>"
"<form method='POST' action='/save'>"
"<label>WiFi SSID</label><input name='ssid' required>"
"<label>WiFi Password</label><input name='pass' type='password' required>"
"<label>Telegram bot token</label><input name='tg_token' required>"
"<label>Telegram channel/chat id</label><input name='tg_chat' required>"
"<label>Ping IP 1 (required)</label><input name='ping_ip' placeholder='192.168.1.1' required>"
"<label>Ping IP 2 (optional)</label><input name='ping_ip2' placeholder='1.1.1.1'>"
"<label>Ping IP 3 (optional)</label><input name='ping_ip3' placeholder='8.8.8.8'>"
"<button type='submit'>Save & Reboot</button>"
"</form></body></html>";

static esp_err_t http_root_get(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, HTML_FORM, HTTPD_RESP_USE_STRLEN);
}

// very small parser for application/x-www-form-urlencoded
static void form_get(const char *body, const char *key, char *out, size_t out_sz) {
    out[0] = 0;
    size_t key_len = strlen(key);

    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        const char *amp = strchr(eq + 1, '&');
        size_t klen = (size_t)(eq - p);
        if (klen == key_len && strncmp(p, key, key_len) == 0) {
            const char *vstart = eq + 1;
            size_t vlen = amp ? (size_t)(amp - vstart) : strlen(vstart);
            // decode minimal: + -> space, %XX
            size_t oi = 0;
            for (size_t i = 0; i < vlen && oi + 1 < out_sz; i++) {
                char c = vstart[i];
                if (c == '+') out[oi++] = ' ';
                else if (c == '%' && i + 2 < vlen) {
                    char h1 = vstart[i+1], h2 = vstart[i+2];
                    int v = 0;
                    if (h1 >= '0' && h1 <= '9') v = (h1 - '0') << 4;
                    else if (h1 >= 'A' && h1 <= 'F') v = (h1 - 'A' + 10) << 4;
                    else if (h1 >= 'a' && h1 <= 'f') v = (h1 - 'a' + 10) << 4;
                    if (h2 >= '0' && h2 <= '9') v |= (h2 - '0');
                    else if (h2 >= 'A' && h2 <= 'F') v |= (h2 - 'A' + 10);
                    else if (h2 >= 'a' && h2 <= 'f') v |= (h2 - 'a' + 10);
                    out[oi++] = (char)v;
                    i += 2;
                } else {
                    out[oi++] = c;
                }
            }
            out[oi] = 0;
            return;
        }
        p = amp ? (amp + 1) : NULL;
    }
}

static esp_err_t http_save_post(httpd_req_t *req) {
    char buf[512];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad content length");
        return ESP_FAIL;
    }

    int r = httpd_req_recv(req, buf, total);
    if (r <= 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Recv failed");
        return ESP_FAIL;
    }
    buf[r] = 0;

    app_cfg_t cfg = {0};
    form_get(buf, "ssid", cfg.ssid, sizeof(cfg.ssid));
    form_get(buf, "pass", cfg.pass, sizeof(cfg.pass));
    form_get(buf, "tg_token", cfg.tg_token, sizeof(cfg.tg_token));
    form_get(buf, "tg_chat", cfg.tg_chat, sizeof(cfg.tg_chat));
    form_get(buf, "ping_ip", cfg.ping_ip, sizeof(cfg.ping_ip));
    form_get(buf, "ping_ip2", cfg.ping_ip2, sizeof(cfg.ping_ip2));
    form_get(buf, "ping_ip3", cfg.ping_ip3, sizeof(cfg.ping_ip3));

    if (strlen(cfg.ssid) == 0 || strlen(cfg.pass) == 0 ||
        strlen(cfg.tg_token) == 0 || strlen(cfg.tg_chat) == 0 ||
        strlen(cfg.ping_ip) == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing fields");
        return ESP_FAIL;
    }

    // Validate required target and optional targets only when filled in.
    ip4_addr_t ip;
    if (!ip4addr_aton(cfg.ping_ip, &ip)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Ping IP 1");
        return ESP_FAIL;
    }
    if (cfg.ping_ip2[0] != 0 && !ip4addr_aton(cfg.ping_ip2, &ip)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Ping IP 2");
        return ESP_FAIL;
    }
    if (cfg.ping_ip3[0] != 0 && !ip4addr_aton(cfg.ping_ip3, &ip)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Ping IP 3");
        return ESP_FAIL;
    }

    if (!save_cfg_to_nvs(&cfg)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS save failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><h3>Saved. Rebooting...</h3></body></html>");
    vTaskDelay(pdMS_TO_TICKS(300));
    reboot_now();
    return ESP_OK;
}

static void start_http_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 6144;

    if (httpd_start(&g_httpd, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start httpd");
        g_httpd = NULL;
        return;
    }

    httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = http_root_get };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = http_save_post };

    httpd_register_uri_handler(g_httpd, &root);
    httpd_register_uri_handler(g_httpd, &save);

    ESP_LOGI(TAG, "HTTP server started");
}

static void wifi_start_ap_for_setup(void) {
    ESP_LOGW(TAG, "Starting AP setup mode... SSID=%s PASS=%s", AP_SSID, AP_PASS);

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap_config = {0};
    strncpy((char*)ap_config.ap.ssid, AP_SSID, sizeof(ap_config.ap.ssid));
    strncpy((char*)ap_config.ap.password, AP_PASS, sizeof(ap_config.ap.password));
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_config.ap.max_connection = AP_MAX_CONN;
    ap_config.ap.channel = 6;
    ap_config.ap.ssid_len = 0;

    if (strlen(AP_PASS) == 0) ap_config.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_http_server();

    ESP_LOGI(TAG, "Connect to AP '%s' and open http://192.168.4.1/", AP_SSID);
}

/* ---------- WiFi STA ---------- */
static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {

        // ✅ ВАЖЛИВО: Wi-Fi down -> скидаємо CONNECTED_BIT
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (s_retry_num < WIFI_MAX_RETRIES) {
            s_retry_num++;
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d", s_retry_num, WIFI_MAX_RETRIES);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }

    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_retry_num = 0;

        // ✅ Wi-Fi up (є IP) -> ставимо CONNECTED_BIT
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_start_sta_or_reboot(const char *ssid, const char *pass) {
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {0};
    strncpy((char*)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    // ✅ Менше "дір" по Wi-Fi, менше шансів на ложні спрацювання
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Connecting to WiFi '%s' ...", ssid);

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(20000)
    );

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected");
        return;
    }

    ESP_LOGE(TAG, "WiFi failed after %d retries -> reboot", WIFI_MAX_RETRIES);
    reboot_now();
}

/* ---------- NTP ---------- */
static void ntp_sync_time_or_reboot(void) {
    ESP_LOGI(TAG, "Starting SNTP...");
    set_tz_kyiv();

    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    const int max_wait_ms = 15000;
    int waited = 0;
    while (waited < max_wait_ms) {
        time_t now;
        time(&now);
        if (now > 1700000000) {
            ESP_LOGI(TAG, "Time synced: %lld", (long long)now);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }

    ESP_LOGE(TAG, "NTP sync timeout -> reboot");

    reboot_now();
}

/* ---------- PING ---------- */
static bool any_latest_ping_ok(void) {
    for (int i = 0; i < MAX_PING_TARGETS; i++) {
        if (g_ping_targets[i].enabled && g_ping_targets[i].last_result) return true;
    }
    return false;
}

static bool any_target_confirmed_on(void) {
    int64_t now = now_ms();
    for (int i = 0; i < MAX_PING_TARGETS; i++) {
        ping_target_t *t = &g_ping_targets[i];
        if (t->enabled && t->ok_streak >= ON_CONFIRM_STREAK &&
            t->last_ok_ms > 0 && (now - t->last_ok_ms) < OFF_CONFIRM_MS) {
            return true;
        }
    }
    return false;
}

static void update_ping_ui_from_aggregate(void) {
    if (!inited) return;
    bool recent = false;
    int64_t last_ok = g_last_ping_ok_ms;
    if (last_ok > 0 && (now_ms() - last_ok) < OFF_CONFIRM_MS) recent = true;
    display_ui_set_last_ping_try(now_epoch_s());
    display_ui_set_ping(recent);
}

static void ping_on_success(esp_ping_handle_t hdl, void *args) {
    (void)hdl;
    ping_target_t *t = (ping_target_t *)args;
    if (!t) return;

    int64_t now = now_ms();
    t->last_ok_ms = now;
    t->last_result = true;
    if (t->ok_streak < 1000) t->ok_streak++;

    g_last_ping_ok_ms = now;
    last_ping_rez = true;

    ESP_LOGD(TAG, "Ping OK: %s (streak=%u)", t->ip ? t->ip : "?", (unsigned)t->ok_streak);
    update_ping_ui_from_aggregate();
}

static void ping_on_timeout(esp_ping_handle_t hdl, void *args) {
    (void)hdl;
    ping_target_t *t = (ping_target_t *)args;
    if (!t) return;

    t->last_result = false;
    // Preserve the old anti-flap behavior: one timeout only reduces the confirmation streak.
    if (t->ok_streak > 0) t->ok_streak--;

    last_ping_rez = any_latest_ping_ok();
    ESP_LOGD(TAG, "Ping timeout: %s", t->ip ? t->ip : "?");
    update_ping_ui_from_aggregate();
}

static esp_err_t ping_start_one(int index, const char *ip_str) {
    if (index < 0 || index >= MAX_PING_TARGETS || !ip_str || ip_str[0] == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ip_addr_t target_addr;
    if (!ipaddr_aton(ip_str, &target_addr)) {
        ESP_LOGE(TAG, "Bad ping ip: %s", ip_str);
        return ESP_ERR_INVALID_ARG;
    }

    ping_target_t *t = &g_ping_targets[index];
    memset(t, 0, sizeof(*t));
    t->ip = ip_str;
    t->enabled = true;

    esp_ping_config_t ping_config = ESP_PING_DEFAULT_CONFIG();
    ping_config.target_addr = target_addr;
    ping_config.count = 0; // infinite
    ping_config.interval_ms = PING_INTERVAL_MS;
    ping_config.timeout_ms = 1500;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = ping_on_success,
        .on_ping_timeout = ping_on_timeout,
        .on_ping_end = NULL,
        .cb_args = t
    };

    esp_err_t err = esp_ping_new_session(&ping_config, &cbs, &t->handle);
    if (err != ESP_OK) {
        t->enabled = false;
        ESP_LOGE(TAG, "Cannot create ping session for %s: %s", ip_str, esp_err_to_name(err));
        return err;
    }

    err = esp_ping_start(t->handle);
    if (err != ESP_OK) {
        t->enabled = false;
        ESP_LOGE(TAG, "Cannot start ping session for %s: %s", ip_str, esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Ping target %d enabled: %s", index + 1, ip_str);
    return ESP_OK;
}

static esp_err_t ping_start_all(void) {
    memset(g_ping_targets, 0, sizeof(g_ping_targets));
    g_last_ping_ok_ms = 0;
    g_ping_monitor_started_ms = now_ms();
    last_ping_rez = false;

    const char *ips[MAX_PING_TARGETS] = {
        g_cfg.ping_ip,
        g_cfg.ping_ip2,
        g_cfg.ping_ip3
    };

    int started = 0;
    for (int i = 0; i < MAX_PING_TARGETS; i++) {
        if (!ips[i] || ips[i][0] == 0) {
            ESP_LOGI(TAG, "Ping target %d disabled (empty)", i + 1);
            continue;
        }
        esp_err_t err = ping_start_one(i, ips[i]);
        if (err != ESP_OK) return err;
        started++;
    }

    return started > 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}

/* ---------- LOGIC: power state machine ---------- */
static void format_duration(int64_t sec, char *out, size_t out_sz) {
    if (sec < 0) sec = 0;
    int64_t h = sec / 3600;
    int64_t m = (sec % 3600) / 60;
    int64_t s = sec % 60;
    if (h > 0) snprintf(out, out_sz, "%" PRId64 "г %" PRId64 "хв %" PRId64 "с", h, m, s);
    else if (m > 0) snprintf(out, out_sz, "%" PRId64 "хв %" PRId64 "с", m, s);
    else snprintf(out, out_sz, "%" PRId64 "с", s);
}

static void notify_power_off(int64_t off_time, int64_t last_on_time) {
    char dur[64]; format_duration(off_time - last_on_time, dur, sizeof(dur));

    char msg[256];
    snprintf(msg, sizeof(msg),
             "❌ Світло зникло.\n"
             "Воно було: %s",
             dur);

    (void)tg_send_message(g_cfg.tg_token, g_cfg.tg_chat, msg);
}

static void notify_power_on(int64_t on_time, int64_t last_off_time) {
    char dur[64]; format_duration(on_time - last_off_time, dur, sizeof(dur));

    char msg[256];
    snprintf(msg, sizeof(msg),
             "✅ Світло з'явилося.\n"
             "Його не було: %s",
             dur);

    (void)tg_send_message(g_cfg.tg_token, g_cfg.tg_chat, msg);
}

// At least one target has replied during the OFF confirmation window.
static bool ping_recent_ok(void) {
    int64_t last_ok = g_last_ping_ok_ms;
    if (last_ok == 0) return false;
    return (now_ms() - last_ok) < OFF_CONFIRM_MS;
}

// OFF is allowed only after a full OFF_CONFIRM_MS window without ANY successful reply.
// After reboot this also gives the monitor a full two-minute grace period before declaring OFF.
static bool no_ping_for_off_window(void) {
    int64_t now = now_ms();
    int64_t last_ok = g_last_ping_ok_ms;
    if (last_ok > 0) return (now - last_ok) >= OFF_CONFIRM_MS;
    return g_ping_monitor_started_ms > 0 && (now - g_ping_monitor_started_ms) >= OFF_CONFIRM_MS;
}

static void main_logic_task(void *arg) {
    (void)arg;

    display_ui_toast("   Obtaining current state...");

    ESP_ERROR_CHECK(ping_start_all());

    int64_t n_s;

    // Wait briefly for any configured target to confirm ON.
    int waited = 0;
    while (!any_target_confirmed_on() && waited < 8000) {
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
        // n_s = now_epoch_s();
        // display_ui_set_last_ping_try(n_s);
    }

    EventBits_t bits0 = xEventGroupGetBits(s_wifi_event_group);
    bool wifi_ok0 = (bits0 & WIFI_CONNECTED_BIT);

    uint8_t st = load_last_status(0xFF);
    bool confirmed_on = wifi_ok0 && any_target_confirmed_on() && ping_recent_ok();
    // Do not turn a previously-ON device OFF just because no target answered in the first few seconds after reboot.
    // Runtime OFF detection below still requires the full two-minute no-success window.
    bool is_on = confirmed_on || (st == 1 && wifi_ok0);

    n_s = now_epoch_s();
    int64_t l_on = load_i64(KEY_LAST_ON_TIME, n_s);
    int64_t l_of = load_i64(KEY_LAST_OFF_TIME, n_s);

    if (is_on) {
        display_ui_set_power(is_on, l_on);
        ui_set_screen_bg(0x009900);
    } else {
        display_ui_set_power(is_on, l_of);
        ui_set_screen_bg(0x990000);
    }
    
    display_ui_set_setup_mode(false);
    display_ui_set_wifi(wifi_ok0);
    display_ui_set_ping(ping_recent_ok());

    
    
    ESP_LOGI(TAG, "Initial power status: %s, last_st=%u", is_on ? "ON" : "OFF", st);
    inited = true;
    // Первый запуск: просто запомнить, без уведомлений
    if (st == 0xFF) {
        st = is_on ? 1 : 0;
        save_last_status(st);
    } else {
        // После ребута уведомлять только если статус реально изменился
        if ((st == 1 && !is_on) || (st == 0 && is_on)) {
            int64_t now_s = now_epoch_s();
            if (!is_on) {
                int64_t last_on = load_i64(KEY_LAST_ON_TIME, now_s);
                save_i64(KEY_LAST_OFF_TIME, now_s);
                notify_power_off(now_s, last_on);
                st = 0;
                save_last_status(st);
                ui_set_screen_bg(0x990000);
                display_ui_set_power(false, last_on);
                ESP_LOGW(TAG, "POWER OFF event (boot)");
            } else {
                int64_t last_off = load_i64(KEY_LAST_OFF_TIME, now_s);
                save_i64(KEY_LAST_ON_TIME, now_s);
                notify_power_on(now_s, last_off);
                st = 1;
                save_last_status(st);
                ui_set_screen_bg(0x009900);
                display_ui_set_power(true, last_off);
                ESP_LOGW(TAG, "POWER ON event (boot)");
            }
        }
    }

    // ✅ Основной цикл:
    // - НЕ міняємо стан, якщо Wi-Fi down
    // - OFF тільки після OFF_CONFIRM_MS без успішного ping
    // - ON тільки якщо є streak і "свіжий" ping
    bool warning = false;

    while (1) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        bool wifi_ok = (bits & WIFI_CONNECTED_BIT);

        if (!wifi_ok) {
            // Wi-Fi down: пауза детекції, стан НЕ міняємо
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        bool evidence_on  = any_target_confirmed_on() && ping_recent_ok();
        bool evidence_off = no_ping_for_off_window();

        if (st == 1 && last_ping_rez == false && !warning) {
            warning = true;
            ui_set_screen_bg(0x999900);
        } else {
            if (st == 1 && last_ping_rez == true && warning) ui_set_screen_bg(0x009900);
            warning = false;
        }

        if (st == 1 && evidence_off) {
            int64_t off_time = now_epoch_s();
            int64_t last_on = load_i64(KEY_LAST_ON_TIME, off_time);
            save_i64(KEY_LAST_OFF_TIME, off_time);
            notify_power_off(off_time, last_on);
            st = 0;
            save_last_status(st);
            ESP_LOGW(TAG, "POWER OFF event (confirmed %d ms with no successful ping from any target)", OFF_CONFIRM_MS);
            ui_set_screen_bg(0x990000);
            display_ui_set_power(false, off_time);

        } else if (st == 0 && evidence_on) {
            int64_t on_time = now_epoch_s();
            int64_t last_off = load_i64(KEY_LAST_OFF_TIME, on_time);
            save_i64(KEY_LAST_ON_TIME, on_time);
            notify_power_on(on_time, last_off);
            st = 1;
            save_last_status(st);
            ESP_LOGW(TAG, "POWER ON event");
            ui_set_screen_bg(0x009900);
            display_ui_set_power(true, on_time);
        }

        display_ui_set_wifi(wifi_ok);
        // display_ui_set_ping(ping_recent_ok());

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---------- button check ---------- */
static bool is_button_held_at_boot(void) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);

    int held_ms = 0;
    while (held_ms < BTN_HOLD_MS) {
        int level = gpio_get_level(BTN_GPIO); // BOOT обычно active-low
        if (level != 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
        held_ms += 20;
    }
    return true;
}

/* ---------- app_main ---------- */
void app_main(void) {
    ESP_ERROR_CHECK(nvs_flash_init());
    dump_all_nvs_entries();
    ESP_LOGI(TAG, "Boot");

    // Настрой под свои пины!
    lcd_st7789_cfg_t lcd = {
        .pin_mosi = 19,
        .pin_sclk = 18,
        .pin_cs   = 5,
        .pin_dc   = 16,
        .pin_rst  = 23,
        .pin_bl   = 4,

        .spi_host = SPI2_HOST,

        // ВАЖЛИВО: для TTGO базова геометрія 240x135
        .hor_res = 135,
        .ver_res = 240,

        // clip window для T-Display
        .x_offset = 40,
        .y_offset = 53,      // якщо зсунуто — спробуй 53

        .pclk_hz = 40 * 1000 * 1000,   // для стабільності; потім можна 40MHz

        .swap_xy  = false,
        .mirror_x = false,
        .mirror_y = false,

        .invert_color = true,           // для T-Display часто треба

        .lvgl_buf_pixels = 240 * 40,
        .double_buffer = false,
    };

    ESP_LOGI(TAG, "LCD init start");
    ESP_ERROR_CHECK(lcd_st7789_init_and_register_lvgl(&lcd));
    ESP_LOGI(TAG, "LCD init done");
    display_ui_init();
    display_ui_toast("               Initializing...");

    vTaskDelay(pdMS_TO_TICKS(3000));
    bool force_setup = is_button_held_at_boot();
    bool has_cfg = load_cfg_from_nvs(&g_cfg);

    if (force_setup || !has_cfg) {
        display_ui_toast("      Entering config mode...");
        ESP_LOGW(TAG, "Entering setup mode. force=%d has_cfg=%d", (int)force_setup, (int)has_cfg);
        vTaskDelay(pdMS_TO_TICKS(5000));
        display_ui_toast("            CONFIG MODE");
        // отдельная инициализация netif/loop для AP
        ESP_ERROR_CHECK(esp_netif_init());
        ESP_ERROR_CHECK(esp_event_loop_create_default());

        // display_ui_set_setup_mode(true);
        // display_ui_set_wifi(false);
        // display_ui_set_ping(false);
        // display_ui_set_power(false, 0);



        wifi_start_ap_for_setup();

        // Просто живем в setup режиме, пока пользователь не сохранит настройки (там reboot)
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    vTaskDelay(pdMS_TO_TICKS(2000));

    // обычный режим

    display_ui_toast("    Establishing connection...");
    wifi_start_sta_or_reboot(g_cfg.ssid, g_cfg.pass);
    ntp_sync_time_or_reboot();

    // Если нет last_on_time/last_off_time, инициализируем текущим временем, чтобы длительности были адекватны
    int64_t t = now_epoch_s();
    if (load_i64(KEY_LAST_ON_TIME, -1) < 0) save_i64(KEY_LAST_ON_TIME, t);
    if (load_i64(KEY_LAST_OFF_TIME, -1) < 0) save_i64(KEY_LAST_OFF_TIME, t);

    xTaskCreate(main_logic_task, "main_logic", 8192, NULL, 5, NULL);
}
