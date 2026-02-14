#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool setup_mode;         // true: AP + Web UI
    bool wifi_ok;            // STA got IP
    bool ping_recent_ok;     // "живой" ping по твоей логике
    bool power_on;           // рассчитанный ON/OFF (твоя стейт-машина)
    int64_t last_change_epoch_s; // время последней смены состояния (epoch seconds), 0 если неизвестно
    int64_t last_ping_try_epoch_s;
} display_status_t;

// Инициализация UI (создает экран, лейблы и т.п.)
void display_ui_init(void);

// Мгновенно обновить весь статус одной структурой (удобно дергать из логики)
void display_ui_set_status(const display_status_t *st);

// Частичные апдейты (если хочешь)
void display_ui_set_setup_mode(bool setup);
void display_ui_set_wifi(bool ok);
void display_ui_set_ping(bool recent_ok);
void display_ui_set_power(bool on, int64_t last_change_epoch_s);
void display_ui_set_last_ping_try(int64_t last_change_epoch_s);

void ui_set_screen_bg(uint32_t hex_rgb);


// Показать краткое сообщение (например “Saved. Rebooting…”)
void display_ui_toast(const char *msg);

#ifdef __cplusplus
}
#endif
