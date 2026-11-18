/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_log.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_wifi.h"

#include "esp_check.h"
#include "driver/gpio.h"
#include "driver/rmt_tx.h"

static const char *TAG = "coprocessor";

#if CONFIG_CP_RGB_LED_GPIO >= 0
/* The dev board's WS2812 latches whatever noise it sees on its data line
 * around reset and can come up lit. Send one "off" frame (24 zero bits). */
static void rgb_led_off(void)
{
    const gpio_num_t pin = CONFIG_CP_RGB_LED_GPIO;
    rmt_channel_handle_t chan = NULL;
    rmt_encoder_handle_t enc = NULL;
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = pin,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10000000,      // 0.1 us per tick
        .mem_block_symbols = 48,
        .trans_queue_depth = 1,
    };
    rmt_copy_encoder_config_t enc_cfg = {};
    rmt_transmit_config_t tx_cfg = {};
    rmt_symbol_word_t bits[24];

    for (int i = 0; i < 24; i++) {
        // WS2812 "0" bit: 0.3 us high, 0.9 us low
        bits[i] = (rmt_symbol_word_t){ .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 };
    }
    if (rmt_new_tx_channel(&chan_cfg, &chan) != ESP_OK) {
        return;
    }
    if (rmt_new_copy_encoder(&enc_cfg, &enc) == ESP_OK &&
        rmt_enable(chan) == ESP_OK) {
        rmt_transmit(chan, enc, bits, sizeof(bits), &tx_cfg);
        rmt_tx_wait_all_done(chan, 10);
        rmt_disable(chan);
    }
    if (enc) {
        rmt_del_encoder(enc);
    }
    rmt_del_channel(chan);

    // hold the data line low (idle) so it doesn't pick up noise
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "ESP-Hosted coprocessor starting");

    /* Initialize NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* create default event loop */
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* Initialise Wi-Fi here with IDF defaults, so the host doesn't have to
     * send a full wifi_init_config_t over RPC. A later host WifiInit with the
     * same parameters is accepted as a no-op by esp_hosted. */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

#if CONFIG_CP_RGB_LED_GPIO >= 0
    /* after esp_wifi_init, so it doesn't delay Wi-Fi being ready for the host */
    rgb_led_off();
#endif
}
