#pragma once
#include <stdbool.h>
#include "esp_err.h"

// Connect as station; blocks until an IPv4 address is assigned or timeout_ms passes.
esp_err_t wifi_connect(const char *ssid, const char *password, int timeout_ms);
bool wifi_is_connected(void);
