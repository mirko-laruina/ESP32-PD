#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef void (*wifi_manager_ip_callback_t)(const char *ip);

esp_err_t wifi_manager_set_ip_callback(wifi_manager_ip_callback_t callback);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
bool wifi_manager_is_connected(void);
const char *wifi_manager_ip(void);
const char *wifi_manager_ssid(void);
