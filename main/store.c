#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "store.h"

static const char *TAG = "APP_STORE";

#define WIFI_CRED_NAMESPACE "wifi_cfg"
#define WIFI_CRED_SSID_KEY  "home_ssid"
#define WIFI_CRED_PASS_KEY  "home_pass"

esp_err_t init_store(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "failed to nvs_flash_erase");
        ESP_RETURN_ON_ERROR(nvs_flash_init(), TAG, "nvs_flash_init after erase failed");
        return ESP_OK;
    }

    return ret;
}

esp_err_t save_credentials_to_nvs(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_CRED_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_str(nvs, WIFI_CRED_SSID_KEY, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, WIFI_CRED_PASS_KEY, pass);
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }

    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved home Wi‑Fi credentials: ssid='%s'", ssid);
    } else {
        ESP_LOGE(TAG, "Failed to save credentials: %s", esp_err_to_name(err));
    }

    return err;
}