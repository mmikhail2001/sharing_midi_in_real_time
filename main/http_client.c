#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"

#include "http_client.h"
#include "midi.h"

#define MIDI_SERVER_URL    "http://45.81.35.86:8080/api/midi"
#define SEND_INTERVAL_MS   6000 // 6s
#define BATCH_SIZE         10                                    // Send 10 at once

static const char *TAG = "APP_HTTP_CLIENT";

// ------------------- JSON serialization -------------------

static char* batch_to_json(midi_message_t* batch, int count)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    
    cJSON *messages = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *msg_json = cJSON_CreateObject();
        cJSON *data = cJSON_CreateArray();
        
        for (int j = 0; j < 4; j++) {
            cJSON_AddItemToArray(data, cJSON_CreateNumber(batch[i].data[j]));
        }
        cJSON_AddItemToObject(msg_json, "data", data);
        cJSON_AddNumberToObject(msg_json, "local_us", batch[i].local_ns);
        
        cJSON_AddItemToArray(messages, msg_json);
    }
    
    cJSON_AddItemToObject(root, "messages", messages);
    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json_str;
}

// ------------------- HTTP POST task -------------------

void http_client_task(void *arg)
{
    midi_message_t batch[BATCH_SIZE];
    int batch_count = 0;
    TickType_t last_send = xTaskGetTickCount();
    
    ESP_LOGI(TAG, "HTTP MIDI sender started, queue msgs: %d", uxQueueMessagesWaiting(midi_queue));
    
    while (true) {
        // Try to read from queue (non-blocking)
        midi_message_t msg;
        if (xQueueReceive(midi_queue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
            // Add to batch
            if (batch_count < BATCH_SIZE) {
                batch[batch_count++] = msg;
            }
        }
        
        // Send batch if full OR interval elapsed AND queue not empty
        TickType_t now = xTaskGetTickCount();
        bool should_send = (batch_count >= BATCH_SIZE) || 
                            ((now - last_send) >= pdMS_TO_TICKS(SEND_INTERVAL_MS));
        
        if (!(should_send && batch_count > 0)) {
            continue;
        }

        char *json_payload = batch_to_json(batch, batch_count);
        if (json_payload == NULL) {
            ESP_LOGE(TAG, "Failed to create JSON");
            batch_count = 0;
            continue;
        }
        
        // HTTP POST
        esp_http_client_config_t config = {
            .url = MIDI_SERVER_URL,
            .method = HTTP_METHOD_POST,
        };
        
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (client) {
            esp_http_client_set_header(client, "Content-Type", "application/json");
            esp_http_client_set_post_field(client, json_payload, strlen(json_payload));
            
            esp_err_t err = esp_http_client_perform(client);
            if (err == ESP_OK) {
                int status = esp_http_client_get_status_code(client);
                ESP_LOGI(TAG, "Sent %d MIDI msgs, HTTP %d (%d queued)", 
                            batch_count, status, uxQueueMessagesWaiting(midi_queue));
            } else {
                ESP_LOGW(TAG, "HTTP failed: %s (%d queued)", 
                            esp_err_to_name(err), uxQueueMessagesWaiting(midi_queue));
            }
            esp_http_client_cleanup(client);
        }
        
        free(json_payload);
        batch_count = 0;
        last_send = now;
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}