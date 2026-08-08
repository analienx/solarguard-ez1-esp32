#include "wifi_manager.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define CONNECTED_BIT BIT0

static const char *TAG = "wifi";
static EventGroupHandle_t bits;

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(bits, CONNECTED_BIT);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(bits, CONNECTED_BIT);
        ESP_LOGI(TAG, "local Wi-Fi connected");
    }
}

esp_err_t wifi_manager_start(const watchdog_config_t *config) {
    bits = xEventGroupCreate();
    if (!bits) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) return loop;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    wifi_config_t wifi = {0};
    strlcpy((char *)wifi.sta.ssid, config->wifi_ssid, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, config->wifi_password, sizeof(wifi.sta.password));
    wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi.sta.pmf_cfg.capable = true;
    wifi.sta.pmf_cfg.required = false;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    ESP_ERROR_CHECK(esp_wifi_start());
    return esp_wifi_set_ps(WIFI_PS_NONE);
}

bool wifi_manager_connected(void) {
    return bits && (xEventGroupGetBits(bits) & CONNECTED_BIT) != 0;
}

int wifi_manager_rssi(void) {
    wifi_ap_record_t record;
    return wifi_manager_connected() && esp_wifi_sta_get_ap_info(&record) == ESP_OK
               ? record.rssi
               : 0;
}
