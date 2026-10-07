#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "esp_http_server.h"
#include "esp_ota_ops.h"

#define WIFI_SSID       "RANDOM"
#define WIFI_PASSWORD   "mugdhodzs38"

static const char *TAG = "OTA";

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
) {
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        ESP_LOGI(TAG, "WiFi disconnected. Reconnecting...");
        esp_wifi_connect();

    } else if (event_base == IP_EVENT &&
               event_id == IP_EVENT_STA_GOT_IP) {

        ip_event_got_ip_t *event =
            (ip_event_got_ip_t *)event_data;

        ESP_LOGI(
            TAG,
            "Got IP: " IPSTR,
            IP2STR(&event->ip_info.ip)
        );
    }
}

static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(
        esp_event_loop_create_default()
    );

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg =
        WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(
        esp_wifi_init(&cfg)
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            WIFI_EVENT,
            ESP_EVENT_ANY_ID,
            &wifi_event_handler,
            NULL
        )
    );

    ESP_ERROR_CHECK(
        esp_event_handler_register(
            IP_EVENT,
            IP_EVENT_STA_GOT_IP,
            &wifi_event_handler,
            NULL
        )
    );

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)
    );

    ESP_ERROR_CHECK(
        esp_wifi_set_config(
            WIFI_IF_STA,
            &wifi_config
        )
    );

    ESP_ERROR_CHECK(
        esp_wifi_start()
    );

    ESP_LOGI(TAG, "WiFi initialization complete");
}

static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html =
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
    "<title>ESP32-S3 OTA</title>"
    "</head>"
    "<body>"
    "<h1>ESP32-S3 OTA Update</h1>"

    "<input type=\"file\" id=\"firmware\" accept=\".bin\">"
    "<br><br>"
    "<button onclick=\"uploadFirmware()\">Upload Firmware</button>"

    "<p id=\"status\"></p>"

    "<script>"
    "async function uploadFirmware() {"
        "const fileInput = document.getElementById('firmware');"
        "const status = document.getElementById('status');"

        "if (!fileInput.files.length) {"
            "status.textContent = 'Please select a firmware file.';"
            "return;"
        "}"

        "const file = fileInput.files[0];"

        "if (!file.name.endsWith('.bin')) {"
            "status.textContent = 'Please select a .bin firmware file.';"
            "return;"
        "}"

        "status.textContent = 'Uploading...';"

        "try {"
            "const response = await fetch('/update', {"
                "method: 'POST',"
                "headers: {"
                    "'Content-Type': 'application/octet-stream'"
                "},"
                "body: file"
            "});"

            "const text = await response.text();"

            "if (response.ok) {"
                "status.textContent = text;"
            "} else {"
                "status.textContent = 'OTA failed: ' + text;"
            "}"
        "} catch (error) {"
            "status.textContent = 'Upload error: ' + error;"
        "}"
    "}"
    "</script>"

    "</body>"
    "</html>";

    httpd_resp_set_type(
        req,
        "text/html"
    );

    return httpd_resp_send(
        req,
        html,
        HTTPD_RESP_USE_STRLEN
    );
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Starting OTA update");

    const esp_partition_t *update_partition =
        esp_ota_get_next_update_partition(NULL);

    if (update_partition == NULL) {

        ESP_LOGE(
            TAG,
            "No OTA partition available"
        );

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "No OTA partition"
        );

        return ESP_FAIL;
    }

    ESP_LOGI(
        TAG,
        "Writing to partition: %s",
        update_partition->label
    );

    esp_ota_handle_t ota_handle = 0;

    esp_err_t err = esp_ota_begin(
        update_partition,
        OTA_SIZE_UNKNOWN,
        &ota_handle
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "esp_ota_begin failed: %s",
            esp_err_to_name(err)
        );

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "OTA begin failed"
        );

        return err;
    }

    char buffer[4096];

    int received;
    int total = 0;

    while (1) {

        received = httpd_req_recv(
            req,
            buffer,
            sizeof(buffer)
        );

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }

        if (received <= 0) {
            break;
        }

        err = esp_ota_write(
            ota_handle,
            buffer,
            received
        );

        if (err != ESP_OK) {

            ESP_LOGE(
                TAG,
                "esp_ota_write failed: %s",
                esp_err_to_name(err)
            );

            esp_ota_abort(ota_handle);

            httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "OTA write failed"
            );

            return err;
        }

        total += received;

        ESP_LOGI(
            TAG,
            "Received %d bytes",
            total
        );
    }

    err = esp_ota_end(ota_handle);

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "esp_ota_end failed: %s",
            esp_err_to_name(err)
        );

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "OTA validation failed"
        );

        return err;
    }

    err = esp_ota_set_boot_partition(
        update_partition
    );

    if (err != ESP_OK) {

        ESP_LOGE(
            TAG,
            "Failed to set boot partition: %s",
            esp_err_to_name(err)
        );

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Failed to set boot partition"
        );

        return err;
    }

    ESP_LOGI(
        TAG,
        "OTA successful!"
    );

    ESP_LOGI(
        TAG,
        "Total received: %d bytes",
        total
    );

    httpd_resp_sendstr(
        req,
        "OTA successful. ESP32-S3 will reboot."
    );

    vTaskDelay(
        pdMS_TO_TICKS(1000)
    );

    esp_restart();

    return ESP_OK;
}

static void start_webserver(void)
{
    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    config.max_uri_handlers = 8;

    httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(
        httpd_start(
            &server,
            &config
        )
    );

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };

    httpd_uri_t ota_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = ota_handler,
        .user_ctx = NULL
    };

    ESP_ERROR_CHECK(
        httpd_register_uri_handler(
            server,
            &index_uri
        )
    );

    ESP_ERROR_CHECK(
        httpd_register_uri_handler(
            server,
            &ota_uri
        )
    );

    ESP_LOGI(
        TAG,
        "OTA web server started"
    );
}

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "ESP32-S3 OTA Demo"
    );

    esp_err_t ret =
        nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        ESP_ERROR_CHECK(
            nvs_flash_init()
        );
    }

    ESP_ERROR_CHECK(ret);

    wifi_init();

    vTaskDelay(
        pdMS_TO_TICKS(3000)
    );

    start_webserver();
}
