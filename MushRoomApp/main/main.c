/* WiFi station Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_system.h"
#include "spi_flash_mmap.h"
#include <esp_http_server.h>
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "driver/gpio.h"

#include "lwip/err.h"
#include "lwip/sys.h"

/* The examples use WiFi configuration that you can set via project configuration menu

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define ESP_MAXIMUM_RETRY  CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define H2E_IDENTIFIER ""
#elif CONFIG_ESP_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

#define BLINK_GPIO  CONFIG_BLINK_GPIO
#define BUTTON_GPIO CONFIG_BUTTON_GPIO

#define INDEX_HTML_PATH "/spiffs/index.html"

char index_html[4096];
char response_data[4096];

static uint8_t s_led_state = 0;

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *TAG = "MAIN";

static int s_retry_num = 0;

static void initi_web_page_buffer(void)
{
	esp_vfs_spiffs_conf_t conf = {
	    .base_path = "/spiffs",
	    .partition_label = NULL,
	    .max_files = 5,
	    .format_if_mount_failed = true};

	ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

	DIR* dir = opendir("/spiffs");
	if (!dir) {
	    ESP_LOGE(TAG, "Failed to open directory");
	    return;
	}

	struct dirent* entry;
	while ((entry = readdir(dir)) != NULL) {
	    char path[1024];
	    snprintf(path, sizeof(path), "/spiffs/%s", entry->d_name);

	    struct stat st;
	    if (stat(path, &st)) {
	        ESP_LOGE(TAG, "Failed to stat %s", path);
	        continue;
	    }

	    char* buffer = malloc(st.st_size + 1);
	    if (!buffer) {
	        ESP_LOGE(TAG, "Failed to allocate memory for %s", path);
	        continue;
	    }

	    FILE *fp = fopen(path, "r");
	    if (fread(buffer, st.st_size, 1, fp) == 0) {
	        ESP_LOGE(TAG, "Failed to read %s", path);
	    }
	    buffer[st.st_size] = '\0';  // Null-terminate the buffer
	    fclose(fp);

	    free(buffer);
	}

	closedir(dir);
}

esp_err_t file_get_handler(httpd_req_t *req)
{
    char filepath[1024];

    if (strcmp(req->uri, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "/spiffs/index.html");
    } else {
        snprintf(filepath, sizeof(filepath), "/spiffs%s", req->uri);
    }

    const char *type = "text/plain";
    if (strstr(filepath, ".html")) {
        type = "text/html";
    } else if (strstr(filepath, ".css")) {
        type = "text/css";
    } else if (strstr(filepath, ".js")) {
        type = "text/javascript";
    } // Add more types if needed

    httpd_resp_set_type(req, type);

    FILE* f = fopen(filepath, "r");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open %s: %s", filepath, strerror(errno));
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    char buf[1024];
    int read_len;
    while ((read_len = fread(buf, 1, sizeof(buf), f)) > 0) {
        httpd_resp_send_chunk(req, buf, read_len);
    }

    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);  // Send the last chunk

    return ESP_OK;
}

esp_err_t led_on_handler(httpd_req_t *req)
{
    gpio_set_level(BLINK_GPIO, 1);
    s_led_state = 1;
    return httpd_resp_send(req, HTTPD_200, HTTPD_RESP_USE_STRLEN);
}

esp_err_t led_off_handler(httpd_req_t *req)
{
    gpio_set_level(BLINK_GPIO, 0);
    s_led_state = 0;
    return httpd_resp_send(req, HTTPD_200, HTTPD_RESP_USE_STRLEN);
}

httpd_uri_t uri_on = {
    .uri = "/ledOn",
    .method = HTTP_GET,
    .handler = led_on_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_off = {
    .uri = "/ledOff",
    .method = HTTP_GET,
    .handler = led_off_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_main = {
    .uri = "/",  // Match all URIs
    .method = HTTP_GET,
    .handler = file_get_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_style_get = {
    .uri = "/style.css",  // Match all URIs
    .method = HTTP_GET,
    .handler = file_get_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_w3_get = {
    .uri = "/w3.css",  // Match all URIs
    .method = HTTP_GET,
    .handler = file_get_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_script_get = {
    .uri = "/script.js",  // Match all URIs
    .method = HTTP_GET,
    .handler = file_get_handler,
    .user_ctx = NULL
};

httpd_uri_t uri_logo_get = {
    .uri = "/logo.png",  // Match all URIs
    .method = HTTP_GET,
    .handler = file_get_handler,
    .user_ctx = NULL
};



httpd_handle_t setup_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_handle_t server = NULL;

    if (httpd_start(&server, &config) == ESP_OK)
    {
        //httpd_register_uri_handler(server, &uri_get);
        httpd_register_uri_handler(server, &uri_on);
        httpd_register_uri_handler(server, &uri_off);
        httpd_register_uri_handler(server, &uri_main);
        httpd_register_uri_handler(server, &uri_style_get);
        httpd_register_uri_handler(server, &uri_w3_get);
        httpd_register_uri_handler(server, &uri_script_get);
        httpd_register_uri_handler(server, &uri_logo_get);
    }

    return server;
}

static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = ESP_WIFI_SSID,
            .password = ESP_WIFI_PASS,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (pasword len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = H2E_IDENTIFIER,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
     * number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
     * happened. */
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
        gpio_set_level(BLINK_GPIO, true);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 ESP_WIFI_SSID, ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

	gpio_reset_pin(BLINK_GPIO);
	/* Set the GPIO as a push/pull output */
	gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

	gpio_reset_pin(BUTTON_GPIO);
	/* Set the GPIO as a push/pull output */
	gpio_set_direction(BUTTON_GPIO, GPIO_MODE_INPUT);
	gpio_set_pull_mode(BUTTON_GPIO, GPIO_FLOATING);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    vTaskDelay(pdMS_TO_TICKS(1000));

    s_led_state = 0;
    ESP_LOGI(TAG, "LED Control SPIFFS Web Server is running ... ...\n");
    initi_web_page_buffer();
    setup_server();

    while(true)
    {
    	vTaskDelay(pdMS_TO_TICKS(100));
    	//s_led_state = gpio_get_level(BUTTON_GPIO);
    	//gpio_set_level(BLINK_GPIO, s_led_state);
    	//ESP_LOGW(TAG, "Button level : %d", s_led_state);
    }
}
