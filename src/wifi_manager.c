#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#include "wifi_manager.h"

#define TAG "WIFI"

static bool s_initialized;
static bool s_started;
static bool s_connect_requested;
static bool s_connected;
static char s_ip[16];
static char s_ssid[33];
static wifi_manager_ip_callback_t s_ip_callback;

static const char *wifi_reason_name(uint8_t reason)
{
    switch (reason)
    {
    case WIFI_REASON_AUTH_EXPIRE:
        return "authentication expired";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "association expired";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "4-way handshake timeout";
    case WIFI_REASON_NO_AP_FOUND:
        return "AP not found";
    case WIFI_REASON_AUTH_FAIL:
        return "authentication failed";
    default:
        return "unknown";
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        if (s_connect_requested)
        {
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        s_connected = false;
        s_ip[0] = '\0';
        ESP_LOGW(TAG, "Disconnected from \"%s\": reason=%u (%s)",
                 s_ssid, event->reason, wifi_reason_name(event->reason));

        if (s_connect_requested)
        {
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "Connected to \"%s\", IP: %s", s_ssid, s_ip);

        if (s_ip_callback != NULL)
        {
            s_ip_callback(s_ip);
        }
    }
}

static esp_err_t wifi_manager_init(void)
{
    if (s_initialized)
    {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
    {
        return err;
    }

    if (esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") == NULL &&
        esp_netif_create_default_wifi_sta() == NULL)
    {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_START,
                                     &wifi_event_handler, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                     &wifi_event_handler, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &wifi_event_handler, NULL);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK)
    {
        return err;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK)
    {
        return err;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_manager_set_ip_callback(wifi_manager_ip_callback_t callback)
{
    s_ip_callback = callback;
    return ESP_OK;
}

esp_err_t wifi_manager_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = wifi_manager_init();
    if (err != ESP_OK)
    {
        return err;
    }

    wifi_config_t config = {0};
    strlcpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid));
    if (password != NULL)
    {
        strlcpy((char *)config.sta.password, password, sizeof(config.sta.password));
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    s_connect_requested = true;

    err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err != ESP_OK)
    {
        return err;
    }

    if (!s_started)
    {
        err = esp_wifi_start();
        if (err == ESP_OK)
        {
            s_started = true;
            esp_wifi_set_max_tx_power(34);
        }
        return err;
    }

    return esp_wifi_connect();
}

bool wifi_manager_is_connected(void)
{
    return s_connected;
}

const char *wifi_manager_ip(void)
{
    return s_ip;
}

const char *wifi_manager_ssid(void)
{
    return s_ssid;
}
