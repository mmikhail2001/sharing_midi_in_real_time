#pragma once

esp_err_t save_credentials_to_nvs(const char *ssid, const char *pass);
esp_err_t init_store(void);
