#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_rom_sys.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

/* Heltec WiFi LoRa 32 V3 (ESP32-S3 + SX1262). */
#define TAG "NodoSensor2"
#define FREQ_HZ 915000000UL
#define MAX_PACKET_LENGTH 192
#define PIN_NSS GPIO_NUM_8
#define PIN_DIO1 GPIO_NUM_14
#define PIN_RESET GPIO_NUM_12
#define PIN_BUSY GPIO_NUM_13
#define PIN_SCK GPIO_NUM_9
#define PIN_MISO GPIO_NUM_11
#define PIN_MOSI GPIO_NUM_10

#define CMD_SET_STANDBY        0x80
#define CMD_SET_RX             0x82
#define CMD_SET_PACKET_TYPE    0x8A
#define CMD_SET_RF_FREQUENCY   0x86
#define CMD_SET_TX_PARAMS      0x8E
#define CMD_SET_MODULATION     0x8B
#define CMD_SET_PACKET_PARAMS  0x8C
#define CMD_SET_DIO_IRQ_PARAMS 0x08
#define CMD_GET_IRQ_STATUS     0x12
#define CMD_CLEAR_IRQ_STATUS   0x02
#define CMD_GET_RX_STATUS      0x13
#define CMD_READ_BUFFER        0x1E
#define CMD_SET_BUFFER_BASE    0x8F
#define CMD_SET_REGULATOR      0x96
#define CMD_SET_TCXO           0x97
#define CMD_CALIBRATE          0x89
#define CMD_CALIBRATE_IMAGE    0x98

static spi_device_handle_t radio;
static EventGroupHandle_t wifi_events;
static QueueHandle_t upload_queue;
#define WIFI_CONNECTED_BIT BIT0

typedef struct {
    char timestamp[25];
    float distance_cm;
    bool sensor_ok;
} sensor_packet_t;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg; (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static void wifi_connect(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                    wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                    wifi_event_handler, NULL));
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    wifi_config_t config = { .sta = {
        .scan_method = WIFI_ALL_CHANNEL_SCAN,
        .failure_retry_cnt = 10,
    }};
    strlcpy((char *)config.sta.ssid, CONFIG_NODO_WIFI_SSID, sizeof(config.sta.ssid));
    strlcpy((char *)config.sta.password, CONFIG_NODO_WIFI_PASSWORD, sizeof(config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Conectando a Wi-Fi: %s", CONFIG_NODO_WIFI_SSID);
}

static void upload_reading(const sensor_packet_t *sensor)
{
    if (!(xEventGroupGetBits(wifi_events) & WIFI_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "Sin Wi-Fi; lectura no enviada");
        return;
    }
    char body[192];
    int length = sensor->sensor_ok
        ? snprintf(body, sizeof(body), "{\"node_id\":\"NodoSensor1\",\"timestamp\":\"%s\",\"distance_cm\":%.2f,\"sensor_ok\":true}", sensor->timestamp, sensor->distance_cm)
        : snprintf(body, sizeof(body), "{\"node_id\":\"NodoSensor1\",\"timestamp\":\"%s\",\"distance_cm\":null,\"sensor_ok\":false}", sensor->timestamp);
    if (length < 0 || length >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "JSON demasiado largo"); return;
    }
    esp_http_client_config_t config = {.url = CONFIG_NODO_API_URL, .timeout_ms = 5000};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { ESP_LOGE(TAG, "No se pudo crear cliente HTTP"); return; }
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "X-API-Key", CONFIG_NODO_API_KEY);
    esp_http_client_set_post_field(client, body, length);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Lectura enviada al dashboard");
    } else {
        ESP_LOGW(TAG, "Error HTTP: %s, estado %d", esp_err_to_name(err), status);
    }
    esp_http_client_cleanup(client);
}

static void upload_task(void *argument)
{
    (void)argument;
    sensor_packet_t sensor;
    while (true) {
        if (xQueueReceive(upload_queue, &sensor, portMAX_DELAY) == pdTRUE) {
            upload_reading(&sensor);
        }
    }
}

static esp_err_t wait_busy(void)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (!gpio_get_level(PIN_BUSY)) return ESP_OK;
        esp_rom_delay_us(10);
    }
    return ESP_ERR_TIMEOUT;
}

/* Comandos SX1262 por SPI. El primer byte de una lectura es el status. */
static esp_err_t sx1262_command(uint8_t opcode, const uint8_t *params, size_t params_len,
                                 uint8_t *response, size_t response_len)
{
    uint8_t tx[260] = { opcode }, rx[260] = { 0 };
    size_t total = 1 + params_len + response_len;
    if (total > sizeof(tx)) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "radio ocupada");
    if (params_len) memcpy(tx + 1, params, params_len);
    spi_transaction_t t = {.length = total * 8, .tx_buffer = tx, .rx_buffer = rx};
    ESP_RETURN_ON_ERROR(spi_device_polling_transmit(radio, &t), TAG, "SPI");
    if (response_len) memcpy(response, rx + 1 + params_len, response_len);
    return ESP_OK;
}

static esp_err_t sx1262_read_buffer(uint8_t offset, uint8_t *data, size_t length)
{
    uint8_t tx[3 + MAX_PACKET_LENGTH] = { CMD_READ_BUFFER, offset, 0 };
    uint8_t rx[3 + MAX_PACKET_LENGTH] = { 0 };
    if (length > MAX_PACKET_LENGTH) return ESP_ERR_INVALID_SIZE;
    ESP_RETURN_ON_ERROR(wait_busy(), TAG, "radio ocupada");
    spi_transaction_t t = {.length = (3 + length) * 8, .tx_buffer = tx, .rx_buffer = rx};
    ESP_RETURN_ON_ERROR(spi_device_polling_transmit(radio, &t), TAG, "SPI lectura");
    memcpy(data, rx + 3, length);
    return ESP_OK;
}

static esp_err_t sx1262_configure(void)
{
    const uint8_t dc_dc[] = { 1 }, tcxo[] = { 2, 0, 1, 0x40 }; /* 1.8 V, 5 ms */
    const uint8_t calibration[] = { 0x7F }, image[] = { 0xE1, 0xE9 }, standby[] = { 0 };
    const uint8_t lora[] = { 1 }, pa[] = { 4, 7, 0, 1 }, tx_power[] = { 14, 4 };
    const uint8_t modulation[] = { 7, 4, 1, 0 }; /* SF7, 125 kHz, CR 4/5 */
    const uint8_t packet[] = { 0, 8, 0, 0xFF, 1, 0 }; /* CRC, encabezado explicito */
    const uint8_t buffer[] = { 0, 0 }, irq[] = { 0x02, 0x62, 0x02, 0x62, 0, 0, 0, 0 };
    uint32_t rf = (uint32_t)(((uint64_t)FREQ_HZ << 25) / 32000000UL);
    uint8_t frequency[] = { rf >> 24, rf >> 16, rf >> 8, rf };

    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_REGULATOR, dc_dc, sizeof(dc_dc), NULL, 0), TAG, "DC-DC");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_TCXO, tcxo, sizeof(tcxo), NULL, 0), TAG, "TCXO");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CALIBRATE, calibration, sizeof(calibration), NULL, 0), TAG, "calibracion");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CALIBRATE_IMAGE, image, sizeof(image), NULL, 0), TAG, "calibracion imagen");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_STANDBY, standby, sizeof(standby), NULL, 0), TAG, "standby");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_PACKET_TYPE, lora, sizeof(lora), NULL, 0), TAG, "tipo LoRa");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_RF_FREQUENCY, frequency, sizeof(frequency), NULL, 0), TAG, "frecuencia");
    ESP_RETURN_ON_ERROR(sx1262_command(0x95, pa, sizeof(pa), NULL, 0), TAG, "PA");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_TX_PARAMS, tx_power, sizeof(tx_power), NULL, 0), TAG, "potencia");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_MODULATION, modulation, sizeof(modulation), NULL, 0), TAG, "modulacion");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_PACKET_PARAMS, packet, sizeof(packet), NULL, 0), TAG, "paquete");
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_SET_BUFFER_BASE, buffer, sizeof(buffer), NULL, 0), TAG, "buffer");
    return sx1262_command(CMD_SET_DIO_IRQ_PARAMS, irq, sizeof(irq), NULL, 0);
}

static esp_err_t start_receive(void)
{
    const uint8_t clear[] = { 0xFF, 0xFF }, continuous[] = { 0xFF, 0xFF, 0xFF };
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CLEAR_IRQ_STATUS, clear, sizeof(clear), NULL, 0), TAG, "limpiar IRQ");
    return sx1262_command(CMD_SET_RX, continuous, sizeof(continuous), NULL, 0);
}

static esp_err_t receive_packet(uint8_t *packet, size_t *length)
{
    uint8_t irq[3], status[3];
    const uint8_t clear[] = { 0xFF, 0xFF };
    if (!gpio_get_level(PIN_DIO1)) return ESP_ERR_NOT_FINISHED;
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_GET_IRQ_STATUS, NULL, 0, irq, sizeof(irq)), TAG, "leer IRQ");
    uint16_t flags = ((uint16_t)irq[1] << 8) | irq[2];
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_CLEAR_IRQ_STATUS, clear, sizeof(clear), NULL, 0), TAG, "limpiar IRQ");
    if (!(flags & 0x0002) || (flags & 0x0060)) return ESP_FAIL; /* RX done, sin CRC/header error */
    ESP_RETURN_ON_ERROR(sx1262_command(CMD_GET_RX_STATUS, NULL, 0, status, sizeof(status)), TAG, "estado RX");
    *length = status[1];
    if (!*length || *length >= MAX_PACKET_LENGTH) return ESP_ERR_INVALID_SIZE;
    return sx1262_read_buffer(status[2], packet, *length);
}

static bool parse_packet(const char *text, sensor_packet_t *out)
{
    char node[16], type[16], unit[8], status[16], battery[8];
    int consumed = 0;
    int fields = sscanf(text,
        "{\"node_id\":\"%15[^\"]\",\"timestamp\":\"%24[^\"]\",\"type\":\"%15[^\"]\","
        "\"value\":%f,\"unit\":\"%7[^\"]\",\"status\":\"%15[^\"]\",\"battery\":%7[^}]}%n",
        node, out->timestamp, type, &out->distance_cm, unit, status, battery, &consumed);
    if (fields != 7 || text[consumed] || strcmp(node, "NodoSensor1") || strcmp(type, "distance") ||
        strcmp(unit, "cm") || strcmp(battery, "null") || strlen(out->timestamp) != 20 || !isfinite(out->distance_cm)) return false;
    out->sensor_ok = !strcmp(status, "ok");
    return out->sensor_ok ? out->distance_cm >= 5 && out->distance_cm <= 400 :
           !strcmp(status, "sensor_error") && out->distance_cm == 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando receptor SX1262 con ESP-IDF");
    wifi_connect();
    upload_queue = xQueueCreate(16, sizeof(sensor_packet_t));
    assert(upload_queue != NULL);
    xTaskCreate(upload_task, "upload_task", 4096, NULL, 5, NULL);
    ESP_ERROR_CHECK(gpio_config(&(gpio_config_t){.pin_bit_mask = (1ULL << PIN_DIO1) | (1ULL << PIN_BUSY), .mode = GPIO_MODE_INPUT}));
    ESP_ERROR_CHECK(gpio_config(&(gpio_config_t){.pin_bit_mask = 1ULL << PIN_RESET, .mode = GPIO_MODE_OUTPUT}));
    gpio_set_level(PIN_RESET, 0); vTaskDelay(pdMS_TO_TICKS(10)); gpio_set_level(PIN_RESET, 1); vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &(spi_bus_config_t){.mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCK, .max_transfer_sz = 3 + MAX_PACKET_LENGTH}, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &(spi_device_interface_config_t){.clock_speed_hz = 8000000, .mode = 0, .spics_io_num = PIN_NSS, .queue_size = 1}, &radio));
    ESP_ERROR_CHECK(sx1262_configure());
    ESP_ERROR_CHECK(start_receive());
    ESP_LOGI(TAG, "LoRa listo: 915 MHz, SF7, BW 125 kHz, CR 4/5");
    while (true) {
        uint8_t raw[MAX_PACKET_LENGTH] = { 0 }; size_t length = 0;
        esp_err_t err = receive_packet(raw, &length);
        if (err == ESP_OK) {
            raw[length] = 0; sensor_packet_t sensor = { 0 };
            if (parse_packet((char *)raw, &sensor)) {
                ESP_LOGI(TAG, "Dato: %s, %.2f cm", sensor.timestamp, sensor.distance_cm);
                if (xQueueSend(upload_queue, &sensor, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "Cola de subida llena; lectura descartada");
                }
            }
            else ESP_LOGW(TAG, "Paquete invalido: %s", raw);
            ESP_ERROR_CHECK(start_receive());
        } else if (err != ESP_ERR_NOT_FINISHED) {
            ESP_LOGW(TAG, "Paquete descartado: %s", esp_err_to_name(err));
            ESP_ERROR_CHECK(start_receive());
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
