#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_netif.h"
#include "nvs.h"
#include "esp_mac.h"

#include "esp_http_server.h"

#include "ap.h"
#include "nvs.h"

// Config-mode AP parameters.
#define CONFIG_AP_SSID     "ESP_MIDI_WIFI"
#define CONFIG_AP_PASS     ""
#define CONFIG_AP_CHANNEL  1
#define CONFIG_AP_MAX_CONN 1


static user_config_t passed_config;

static const char *TAG = "APP_WIFI_AP";
static EventGroupHandle_t config_passed_event_group;

#define CONFIG_PASSED_BIT BIT0

static const char *INDEX_HTML =
    "<!DOCTYPE html>"
    "<html>"
    "<head><meta charset=\"utf-8\"><title>WiFi Config</title></head>"
    "<body>"
    "<h1>Configure Home Wi‑Fi</h1>"
    "<form method=\"POST\" action=\"/config\">"
    "  SSID: <input type=\"text\" name=\"ssid\"><br><br>"
    "  Password: <input type=\"password\" name=\"pass\"><br><br>"
    "  <input type=\"submit\" value=\"Save\">"
    "</form>"
    "</body>"
    "</html>";

static void wifi_ap_event_handler(void *arg,
                                  esp_event_base_t event_base,
                                  int32_t event_id,
                                  void *event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *event =
            (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *event =
            (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}

static void wifi_init_config_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_ap_event_handler, NULL, NULL));

    wifi_config_t wifi_cfg = { 0 };
    strncpy((char *)wifi_cfg.ap.ssid, CONFIG_AP_SSID, sizeof(wifi_cfg.ap.ssid));
    wifi_cfg.ap.ssid_len = strlen(CONFIG_AP_SSID);
    strncpy((char *)wifi_cfg.ap.password, CONFIG_AP_PASS,
            sizeof(wifi_cfg.ap.password));
    wifi_cfg.ap.channel = CONFIG_AP_CHANNEL;
    wifi_cfg.ap.max_connection = CONFIG_AP_MAX_CONN;
    wifi_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_cfg.ap.pmf_cfg.required = true;

    if (strlen(CONFIG_AP_PASS) == 0) {
        wifi_cfg.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG,
             "Config AP started. SSID:%s password:%s channel:%d",
             CONFIG_AP_SSID, CONFIG_AP_PASS, CONFIG_AP_CHANNEL);
}

// Minimal parser, same as before.
static void parse_form_body(char *body, char *out_ssid, size_t ssid_len,
                            char *out_pass, size_t pass_len)
{
    out_ssid[0] = '\0';
    out_pass[0] = '\0';

    char *ssid_pos = strstr(body, "ssid=");
    char *pass_pos = strstr(body, "pass=");

    if (ssid_pos) {
        ssid_pos += strlen("ssid=");
        char *end = strchr(ssid_pos, '&');
        size_t len = end ? (size_t)(end - ssid_pos) : strlen(ssid_pos);
        if (len >= ssid_len) len = ssid_len - 1;
        memcpy(out_ssid, ssid_pos, len);
        out_ssid[len] = '\0';
    }

    if (pass_pos) {
        pass_pos += strlen("pass=");
        char *end = strchr(pass_pos, '&');
        size_t len = end ? (size_t)(end - pass_pos) : strlen(pass_pos);
        if (len >= pass_len) len = pass_len - 1;
        memcpy(out_pass, pass_pos, len);
        out_pass[len] = '\0';
    }
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    int total_len = req->content_len;
    int cur_len = 0;
    int received = 0;

    char *buf = malloc(total_len + 1);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    while (cur_len < total_len) {
        received = httpd_req_recv(req, buf + cur_len, total_len - cur_len);
        if (received <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        cur_len += received;
    }
    buf[total_len] = '\0';

    ESP_LOGI(TAG, "Received body: %s", buf);

    char passed_ssid[64];
    char passed_pass[64];
    parse_form_body(buf, passed_ssid, sizeof(passed_ssid), passed_pass, sizeof(passed_pass));
    free(buf);

    const char *resp =
        "<html><body><h1>Saved!</h1>"
        "<p>Device will reboot and connect to your home Wi‑Fi.</p>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);

    memcpy(passed_config.ssid, passed_ssid, strlen(passed_ssid));
    memcpy(passed_config.pass, passed_pass, strlen(passed_pass));

    xEventGroupSetBits(config_passed_event_group, CONFIG_PASSED_BIT);

    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t cfg = {
            .uri = "/config",
            .method = HTTP_POST,
            .handler = config_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &cfg);
    }
    return server;
}

user_config_t* get_user_config(void)
{
    wifi_init_config_ap();
    start_webserver();

    config_passed_event_group = xEventGroupCreate();

    EventBits_t bits = xEventGroupWaitBits(
        config_passed_event_group,
        CONFIG_PASSED_BIT,
        pdFALSE,
        pdFALSE,
        portMAX_DELAY);


    if (bits & CONFIG_PASSED_BIT) {
        return &passed_config;
    }
    return NULL;
}
