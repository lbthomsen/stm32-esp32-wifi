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

static const char *TAG = "coprocessor";

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
}
