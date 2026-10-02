#include "matrix.h"
#include "esp_err.h"
#include "led_strip.h"

#define MATRIX_GPIO 14
#define MATRIX_LEDS 64

static led_strip_handle_t s_strip;

void matrix_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = MATRIX_GPIO,
        .max_leds = MATRIX_LEDS,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_RGB,  // this matrix is RGB, not the usual WS2812 GRB
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    led_strip_clear(s_strip);
}

void matrix_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (int i = 0; i < MATRIX_LEDS; i++) led_strip_set_pixel(s_strip, i, r, g, b);
}

void matrix_set(int row, int col, uint8_t r, uint8_t g, uint8_t b)
{
    if (row >= 0 && row < 8 && col >= 0 && col < 8) led_strip_set_pixel(s_strip, row * 8 + col, r, g, b);
}

void matrix_show(void)
{
    led_strip_refresh(s_strip);
}
