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
static bool started;

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

#define RETURN_ON_ERROR(expression)          \
    do {                                      \
        esp_err_t _err = (expression);        \
        if (_err != ESP_OK) {                 \
            ESP_LOGE(TAG, "%s failed: %s", #expression, esp_err_to_name(_err)); \
            return _err;                      \
        }                                     \
    } while (0)

esp_err_t wifi_manager_start(const watchdog_config_t *config) {
    /* The Wi-Fi driver, default STA netif and event handlers can only be set up
     * once per boot. A second call must not re-initialize or retarget them. */
    if (started) return ESP_ERR_INVALID_STATE;
    if (!bits) bits = xEventGroupCreate();
    if (!bits) return ESP_ERR_NO_MEM;
    RETURN_ON_ERROR(esp_netif_init());
    esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) return loop;
    if (!esp_netif_create_default_wifi_sta()) return ESP_FAIL;
    started = true;
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    RETURN_ON_ERROR(esp_wifi_init(&init));
    RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL));
    wifi_config_t wifi = {0};
    strlcpy((char *)wifi.sta.ssid, config->wifi_ssid, sizeof(wifi.sta.ssid));
    strlcpy((char *)wifi.sta.password, config->wifi_password, sizeof(wifi.sta.password));
    wifi.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi.sta.pmf_cfg.capable = true;
    wifi.sta.pmf_cfg.required = false;
    RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA));
    RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    RETURN_ON_ERROR(esp_wifi_start());
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
