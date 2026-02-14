#include "lcd_st7789.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "lcd_st7789";

static esp_lcd_panel_handle_t s_panel = NULL;
static int s_pin_bl = -1;

static esp_err_t CHECK(esp_err_t err, const char *what)
{
    if (err != ESP_OK) ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    return err;
}

void lcd_st7789_set_backlight(bool on)
{
    if (s_pin_bl < 0) return;
    gpio_set_level(s_pin_bl, on ? 1 : 0);
}

static void bl_init(int pin_bl)
{
    s_pin_bl = pin_bl;
    if (pin_bl < 0) return;

    gpio_config_t io = {
        .pin_bit_mask = 1ULL << pin_bl,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0,
        .pull_down_en = 0,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io);
    gpio_set_level(pin_bl, 1);
}

esp_err_t lcd_st7789_init_and_register_lvgl(const lcd_st7789_cfg_t *cfg)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    bl_init(cfg->pin_bl);

    // 1) SPI bus init
    ESP_LOGI(TAG, "spi_bus_initialize(host=%d)...", cfg->spi_host);

    spi_bus_config_t buscfg = {
        .mosi_io_num = cfg->pin_mosi,
        .miso_io_num = -1,
        .sclk_io_num = cfg->pin_sclk,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)(cfg->hor_res * cfg->ver_res * 2 + 8),
    };

    // Если bus уже был инициализирован — будет ESP_ERR_INVALID_STATE.
    // В таком случае просто продолжаем.
    esp_err_t err = spi_bus_initialize((spi_host_device_t)cfg->spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return CHECK(err, "spi_bus_initialize");
    }

    // 2) Panel IO SPI
    ESP_LOGI(TAG, "esp_lcd_new_panel_io_spi...");

    esp_lcd_panel_io_handle_t io_handle = NULL;

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = cfg->pin_dc,
        .cs_gpio_num = cfg->pin_cs,
        .pclk_hz = cfg->pclk_hz,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
    };

    // В твоей сборке тут ожидается spi_host_device_t (не bus handle!)
    err = esp_lcd_new_panel_io_spi((spi_host_device_t)cfg->spi_host, &io_cfg, &io_handle);
    if (CHECK(err, "esp_lcd_new_panel_io_spi") != ESP_OK) return err;

    // 3) Panel ST7789
    ESP_LOGI(TAG, "esp_lcd_new_panel_st7789...");

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = cfg->pin_rst,
        .rgb_endian = LCD_RGB_ENDIAN_RGB,
        .bits_per_pixel = 16,
    };

    err = esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_panel);
    if (CHECK(err, "esp_lcd_new_panel_st7789") != ESP_OK) return err;

    err = esp_lcd_panel_reset(s_panel);
    if (CHECK(err, "esp_lcd_panel_reset") != ESP_OK) return err;

    err = esp_lcd_panel_init(s_panel);
    if (CHECK(err, "esp_lcd_panel_init") != ESP_OK) return err;
    esp_lcd_panel_disp_on_off(s_panel, true);
    // 4) Panel options
    err = esp_lcd_panel_set_gap(s_panel, cfg->x_offset, cfg->y_offset);
    if (CHECK(err, "esp_lcd_panel_set_gap") != ESP_OK) return err;

    err = esp_lcd_panel_invert_color(s_panel, cfg->invert_color);
    if (CHECK(err, "esp_lcd_panel_invert_color") != ESP_OK) return err;

    err = esp_lcd_panel_mirror(s_panel, cfg->mirror_x, cfg->mirror_y);
    if (CHECK(err, "esp_lcd_panel_mirror") != ESP_OK) return err;

    err = esp_lcd_panel_swap_xy(s_panel, cfg->swap_xy);
    if (CHECK(err, "esp_lcd_panel_swap_xy") != ESP_OK) return err;

    // 5) LVGL init
    ESP_LOGI(TAG, "lvgl_port_init...");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    err = lvgl_port_init(&lvgl_cfg);
    if (CHECK(err, "lvgl_port_init") != ESP_OK) return err;

    // 6) LVGL add display (создаёт default display)
    ESP_LOGI(TAG, "lvgl_port_add_disp...");
    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,          // <-- ВАЖНО для твоей версии esp_lvgl_port
        .panel_handle = s_panel,
        .buffer_size = cfg->lvgl_buf_pixels,
        .double_buffer = cfg->double_buffer,
        .hres = cfg->hor_res,
        .vres = cfg->ver_res,
        .monochrome = false,
        .flags = {
            .swap_bytes = 1, // если цвета странные — попробуй 0
        },
    };

    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    lv_disp_set_rotation(disp, LV_DISP_ROTATION_270);

    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "LCD+LVGL ready: %ux%u gap(%u,%u) pclk=%d",
             cfg->hor_res, cfg->ver_res, cfg->x_offset, cfg->y_offset, cfg->pclk_hz);

    return ESP_OK;
}
