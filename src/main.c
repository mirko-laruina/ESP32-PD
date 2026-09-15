
#include "esp_system.h"
#include "esp_console.h"
#include "esp_flash.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pd_config.h"
#include "pd_rx.h"
#include "pd_tx.h"
#include "pd.h"
#include "webserver.h"

#define TAG "ESP32-PD"

void cmd_init(void);
void cmd_main(void);

static void initialize_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main()
{
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    ESP_LOGI(TAG, "Starting up");
    initialize_nvs();

    ESP_LOGI(TAG, "  * Initialize IDLE");

    /* my test setup has the 5k1 PD connected to a GPIO so i can toggle connect/disconnect */
    pd_mode(PD_MODE_IDLE);
    vTaskDelay(1000 / portTICK_PERIOD_MS);

    pd_init();

    ESP_LOGI(TAG, "  * Web interface");
    webserver_start();

    ESP_LOGI(TAG, "  * Main loop");

    cmd_init();

    cmd_main();
    while (true)
    {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }
}
