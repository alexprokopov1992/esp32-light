#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    // SPI pins
    int pin_mosi;
    int pin_sclk;
    int pin_cs;
    int pin_dc;
    int pin_rst;     // -1 если нет
    int pin_bl;      // -1 если нет (управление подсветкой)

    // SPI host
    int spi_host;    // SPI2_HOST обычно

    // Panel params
    uint16_t hor_res;   // 135
    uint16_t ver_res;   // 240

    // Для 135x240 (1.14") часто нужны offsets:
    // в portrait: x_offset=52? / 40; y_offset=40/53 (зависит от модуля)
    // Типовой вариант для 240x135: x=40, y=53
    uint16_t x_offset;
    uint16_t y_offset;

    // частота SPI для LCD
    int pclk_hz;     // напр. 40*1000*1000

    // поворот / отражения
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    bool invert_color;

    // LVGL буфер (в пикселях), обычно кусок экрана
    uint32_t lvgl_buf_pixels; // например 240*40 или 135*40

    // double buffer
    bool double_buffer;

} lcd_st7789_cfg_t;

// Инициализация LCD + регистрация дисплея в LVGL (через esp_lvgl_port).
esp_err_t lcd_st7789_init_and_register_lvgl(const lcd_st7789_cfg_t *cfg);

// Управление подсветкой (если pin_bl != -1)
void lcd_st7789_set_backlight(bool on);

#ifdef __cplusplus
}
#endif
