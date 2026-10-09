#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "ra01s.h"

#define TRIGGER_PIN GPIO_NUM_7
#define ECHO_PIN GPIO_NUM_6
#define MAX_TIMEOUT_US 30000
#define MIN_VALID_CM 5.0f
#define MAX_VALID_CM 400.0f
#define NO_RESPONSE_LIMIT 3
#define LORA_FREQUENCY_HZ 915000000UL
#define LORA_TX_POWER_DBM 14
#define LORA_SEND_INTERVAL_MS 5000

#if __has_include("wifi_credentials_local.h")
#include "wifi_credentials_local.h"
#else
#define NODE_WIFI_SSID     ""
#define NODE_WIFI_PASSWORD ""
#endif
#define WIFI_CONNECTED_BIT BIT0
#define MIN_VALID_EPOCH 1704067200

static const char *TAG = "NodoSensor1";
static int64_t last_echo_pulse_us;
static EventGroupHandle_t wifi_event_group;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool sync_clock_from_ntp(void)
{
    if (NODE_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Completa NODE_WIFI_SSID y NODE_WIFI_PASSWORD al inicio de main.c");
        return false;
    }

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "No hay memoria para el grupo de eventos WiFi");
        return false;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s",
             NODE_WIFI_SSID);
    snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s",
             NODE_WIFI_PASSWORD);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Conectando a WiFi para sincronizar fecha y hora");
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdTRUE, portMAX_DELAY);
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        ESP_LOGE(TAG, "No fue posible conectar a WiFi");
        return false;
    }

    esp_sntp_config_t sntp_config =
        ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    ESP_ERROR_CHECK(esp_netif_sntp_init(&sntp_config));

    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(30000)) != ESP_OK) {
        ESP_LOGW(TAG, "Esperando sincronizacion NTP...");
    }

    time_t now = time(NULL);
    struct tm utc_time;
    char timestamp[25];
    if (now < MIN_VALID_EPOCH || gmtime_r(&now, &utc_time) == NULL ||
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc_time) == 0) {
        ESP_LOGE(TAG, "El reloj no quedo sincronizado");
        return false;
    }

    ESP_LOGI(TAG, "Reloj UTC sincronizado: %s", timestamp);
    return true;
}

static void init_ultrasonic(void)
{
    gpio_config_t trig = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << TRIGGER_PIN),
        .pull_down_en = GPIO_PULLDOWN_DISABLE, .pull_up_en = GPIO_PULLUP_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&trig));

    gpio_config_t echo = {
        .intr_type = GPIO_INTR_DISABLE, .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << ECHO_PIN),
        .pull_down_en = GPIO_PULLDOWN_ENABLE, .pull_up_en = GPIO_PULLUP_DISABLE
    };
    ESP_ERROR_CHECK(gpio_config(&echo));
    ESP_ERROR_CHECK(gpio_set_level(TRIGGER_PIN, 0));
}

static float get_distance_cm(void)
{
    last_echo_pulse_us = 0;
    gpio_set_level(TRIGGER_PIN, 1);
    esp_rom_delay_us(10);
    gpio_set_level(TRIGGER_PIN, 0);

    int64_t wait_start = esp_timer_get_time();
    while (gpio_get_level(ECHO_PIN) == 0) {
        if (esp_timer_get_time() - wait_start > MAX_TIMEOUT_US) return -1.0f;
    }

    int64_t echo_start = esp_timer_get_time();
    int64_t timeout = echo_start + MAX_TIMEOUT_US;
    while (gpio_get_level(ECHO_PIN) == 1) {
        if (esp_timer_get_time() >= timeout) return -1.0f;
    }

    last_echo_pulse_us = esp_timer_get_time() - echo_start;
    if (last_echo_pulse_us <= 0) return -1.0f;

    float distance = ((float)last_echo_pulse_us * 0.0343f) / 2.0f;
    if (distance < MIN_VALID_CM || distance > MAX_VALID_CM) return -1.0f;
    return distance;
}

static bool init_lora(void)
{
    LoRaInit();
    if (LoRaBegin(LORA_FREQUENCY_HZ, LORA_TX_POWER_DBM, 1.8f, true) != 0) {
        ESP_LOGE(TAG, "No se pudo inicializar el SX1262");
        return false;
    }
    LoRaConfig(7, 4, 1, 8, 0, true, false);
    ESP_LOGI(TAG, "LoRa listo: 915 MHz, SF7, BW125, CR4/5");
    return true;
}

void app_main(void)
{
    float filtered_distance = -1.0f;
    int no_response_count = 0;
    int64_t last_send_us = 0;

    init_ultrasonic();
    ESP_LOGI(TAG, "Sensor ultrasonico inicializado");

    if (!init_lora()) {
        while (true) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    while (!sync_clock_from_ntp()) {
        ESP_LOGW(TAG, "No se enviaran datos hasta tener fecha y hora real");
        vTaskDelay(pdMS_TO_TICKS(10000));
    }

    while (true) {
        float raw_distance = get_distance_cm();
        bool sensor_ok = raw_distance >= 0.0f;

        if (!sensor_ok) {
            if (no_response_count < NO_RESPONSE_LIMIT) no_response_count++;
            if (no_response_count == NO_RESPONSE_LIMIT)
                ESP_LOGW(TAG, "Sensor sin respuesta o fuera de rango");
            filtered_distance = 0.0f;
        } else {
            if (no_response_count >= NO_RESPONSE_LIMIT)
                ESP_LOGI(TAG, "Sensor respondiendo nuevamente");
            no_response_count = 0;
            filtered_distance = filtered_distance <= 0.0f ? raw_distance :
                                (filtered_distance * 0.7f) + (raw_distance * 0.3f);
            ESP_LOGI(TAG, "ECHO=%" PRId64 " us, raw=%.2f cm, filtrada=%.2f cm",
                     last_echo_pulse_us, raw_distance, filtered_distance);
        }

        int64_t now_us = esp_timer_get_time();
        if (now_us - last_send_us >= (int64_t)LORA_SEND_INTERVAL_MS * 1000) {
            time_t now = time(NULL);
            struct tm utc_time;
            char timestamp[25];
            char payload[192];

            if (now >= MIN_VALID_EPOCH && gmtime_r(&now, &utc_time) != NULL &&
                strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ",
                         &utc_time) > 0) {
                int len = snprintf(payload, sizeof(payload),
                    "{\"node_id\":\"NodoSensor1\",\"timestamp\":\"%s\","
                    "\"type\":\"distance\",\"value\":%.2f,\"unit\":\"cm\","
                    "\"status\":\"%s\",\"battery\":null}",
                    timestamp, filtered_distance, sensor_ok ? "ok" : "sensor_error");

                if (len > 0 && len < sizeof(payload)) {
                    if (LoRaSend((uint8_t *)payload, len, SX126x_TXMODE_SYNC))
                        ESP_LOGI(TAG, "Paquete LoRa enviado: %s", payload);
                    else
                        ESP_LOGE(TAG, "Fallo el envio LoRa");
                } else {
                    ESP_LOGE(TAG, "El mensaje JSON excede el buffer");
                }
            } else {
                ESP_LOGW(TAG, "Reloj sin sincronizacion; se omite este paquete");
            }
            last_send_us = now_us;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}