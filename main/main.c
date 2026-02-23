/*
- Способ ввода названия Wi-Fi сети и пароля
    - По какому каналу связи? Физическая клавиатура не будет использоваться
    - Для bluetooth нужно разрабатывать Android/iOS приложение? Нет времени.
- Подключение к Wi-Fi
- Отправка midi-данных и событий МК на сервер (подключили MIDI / отключили и другие)
    - Какую использовать БД и как быстро визуализировать?
- Обновления по воздуху
    - Чтобы можно было перепрошить МК у удаленных клиентов


Received body: ssid=DIR-825-5G&pass=60488527
*/


#include <stdio.h>
#include <stdbool.h>
#include <unistd.h>

#include <sys/time.h>
#include <time.h>
#include "esp_sntp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "esp_system.h"
#include "esp_timer.h"  

#include "driver/gpio.h"

#include "nvs_flash.h"
#include "nvs.h"

#include "midi.h"
#include "ap.h"
#include "sta.h"
#include "http_client.h"

static const char *TAG = "APP_MAIN";

#define WIFI_CRED_NAMESPACE "wifi_cfg"
#define WIFI_CRED_SSID_KEY  "home_ssid"
#define WIFI_CRED_PASS_KEY  "home_pass"
#define BOOT_BUTTON_GPIO GPIO_NUM_0

#define DAEMON_TASK_PRIORITY    2
#define CLASS_TASK_PRIORITY     3

// Check if there are saved credentials in NVS.
static bool credentials_exist_in_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_CRED_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return false;
    }

    // We only care if SSID key exists.
    size_t len = 0;
    err = nvs_get_str(nvs, WIFI_CRED_SSID_KEY, NULL, &len);
    nvs_close(nvs);

    return (err == ESP_OK && len > 0);
}

// Erase credentials (factory reset for Wi‑Fi config).
static void erase_credentials_in_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(WIFI_CRED_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open for erase failed: %s", esp_err_to_name(err));
        return;
    }

    // Remove only our keys.
    nvs_erase_key(nvs, WIFI_CRED_SSID_KEY);
    nvs_erase_key(nvs, WIFI_CRED_PASS_KEY);
    nvs_commit(nvs);
    nvs_close(nvs);

    ESP_LOGI(TAG, "Wi‑Fi credentials erased in NVS");
}

// --- BOOT button interrupt ---

// Queue to communicate from ISR to task (ISR cannot call complex functions)
static QueueHandle_t button_press_queue = NULL;

// Interrupt Service Routine (ISR) - must be minimal and in IRAM
static void IRAM_ATTR boot_button_isr_handler(void* arg)
{
    uint32_t gpio_num = (uint32_t) arg;
    // Notify the task via queue (debouncing done in task)
    xQueueSendFromISR(button_press_queue, &gpio_num, NULL);
}

// Task that handles button press from ISR queue (debounced)
static void button_handler_task(void* arg)
{
    uint32_t io_num;
    uint64_t last_press_time = 0;
    const uint64_t debounce_delay_us = 50000; // 50ms debounce

    while (1) {
        if (xQueueReceive(button_press_queue, &io_num, portMAX_DELAY)) {
            uint64_t now = esp_timer_get_time();
            
            // Simple debounce: ignore presses too close together
            if (now - last_press_time > debounce_delay_us) {
                ESP_LOGI(TAG, "BOOT button pressed -> erasing Wi‑Fi config");
                erase_credentials_in_nvs();
                
                // Brief delay to flush logs, then restart
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            last_press_time = now;
        }
    }
}

static void init_boot_button_interrupt(void)
{
    // Create queue for ISR -> task communication
    button_press_queue = xQueueCreate(10, sizeof(uint32_t));
    if (button_press_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create button queue");
        return;
    }

    // Configure GPIO as input with internal pull-up
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,  // Interrupt on FALLING edge (button press)
    };
    gpio_config(&io_conf);

    // Install ISR service (level 1 priority, auto-clear on edge)
    gpio_install_isr_service(ESP_INTR_FLAG_LEVEL1);
    
    // Hook ISR handler (GPIO0 as argument)
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, boot_button_isr_handler, (void*) BOOT_BUTTON_GPIO);

    // Start handler task
    xTaskCreate(button_handler_task, "button_task", 4096, NULL, 10, NULL);

    ESP_LOGI(TAG, "BOOT button interrupt initialized (GPIO%d, FALLING edge)", BOOT_BUTTON_GPIO);
}

void start_midi(void) {
    midi_queue = xQueueCreate(1000, sizeof(midi_message_t));
    if (midi_queue==NULL) {
        ESP_LOGE(TAG, "midi_queue not created");
        abort();
    }

    // midi
    SemaphoreHandle_t signaling_sem = xSemaphoreCreateBinary();
    TaskHandle_t daemon_task_hdl;
    TaskHandle_t class_driver_task_hdl;
    xTaskCreatePinnedToCore(host_lib_daemon_task,
                            "midi_daemon",
                            4096,
                            (void *)signaling_sem,
                            DAEMON_TASK_PRIORITY,
                            &daemon_task_hdl,
                            0);
    xTaskCreatePinnedToCore(class_driver_task,
                            "midi_class",
                            4096,
                            (void *)signaling_sem,
                            CLASS_TASK_PRIORITY,
                            &class_driver_task_hdl,
                            0);

    vTaskDelay(10);
}

void init_local_time(void)
{
    ESP_LOGI(TAG, "Setting local timezone...");
    
    // Set Moscow MSK (UTC+3)
    setenv("TZ", "MSK-3", 1);
    tzset();

    // Force immediate NTP sync
    sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    
    ESP_LOGI(TAG, "NTP sync started automatically via lwIP...");
    
    // Wait for NTP sync (lwIP does this automatically after WiFi)
    int retry = 0;
    time_t now;
    
    while (retry < 10) {
        time(&now);
        if (now > 8 * 3600 * 2) {  // Reasonable time
            struct tm timeinfo;
            localtime_r(&now, &timeinfo);
            ESP_LOGI(TAG, "Local MSK time: %04d-%02d-%02d %02d:%02d:%02d",
                     timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                     timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    
    ESP_LOGW(TAG, "NTP not synced yet (continues in background)");
}


void app_main(void)
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);

    // Initialize BOOT button interrupt FIRST (works in both modes)
    init_boot_button_interrupt();

    // Decide mode based on NVS contents
    if (credentials_exist_in_nvs()) {
        ESP_LOGI(TAG, "Credentials found, starting STA (home Wi‑Fi)");
        start_wifi_sta_home();
        init_local_time();
        start_midi();
        xTaskCreate(http_client_task, "http_client_task", 8192, NULL, 6, NULL);
    } else {
        ESP_LOGI(TAG, "No credentials, starting AP config (setup mode)");
        start_wifi_ap_config();
    }
}
