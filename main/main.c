#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "nvs_flash.h"

#define WIFI_SSID       "RANDOM"
#define WIFI_PASSWORD   "mugdhodzs38"

#define OTA_BUFFER_SIZE 4096

static const char *TAG = "OTA";


/* ============================================================
 * WiFi
 * ============================================================ */

static void wifi_event_handler(
    void *arg,
    esp_event_base_t event_base,
    int32_t event_id,
    void *event_data
)
{
    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_START) {

        esp_wifi_connect();

    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {

        ESP_LOGI(
            TAG,
            "WiFi disconnected. Reconnecting..."
        );

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
    ESP_ERROR_CHECK(
        esp_netif_init()
    );

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

    ESP_LOGI(
        TAG,
        "WiFi initialization complete"
    );
}


/* ============================================================
 * Web UI
 * ============================================================ */

static esp_err_t index_handler(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html>"
        "<html>"
        "<head>"
        "<meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" "
        "content=\"width=device-width, initial-scale=1.0\">"
        "<title>ESP32-S3 OTA</title>"
        "</head>"

        "<body>"

        "<h1>ESP32-S3 OTA Update v1</h1>"

        "<input "
        "type=\"file\" "
        "id=\"firmware\" "
        "accept=\".bin\">"

        "<br><br>"

        "<button onclick=\"uploadFirmware()\">"
        "Upload Firmware"
        "</button>"

        "<p id=\"status\"></p>"

        "<script>"

        "async function uploadFirmware() {"

            "const fileInput = "
            "document.getElementById('firmware');"

            "const status = "
            "document.getElementById('status');"

            "if (!fileInput.files.length) {"
                "status.textContent = "
                "'Please select a firmware file.';"
                "return;"
            "}"

            "const file = fileInput.files[0];"

            "if (!file.name.toLowerCase().endsWith('.bin')) {"
                "status.textContent = "
                "'Please select a .bin firmware file.';"
                "return;"
            "}"

            "status.textContent = "
            "'Uploading firmware...';"

            "try {"

                "const response = await fetch('/update', {"

                    "method: 'POST',"

                    "headers: {"
                        "'Content-Type': "
                        "'application/octet-stream'"
                    "},"

                    "body: file"

                "});"

                "const text = await response.text();"

                "if (response.ok) {"

                    "status.textContent = text;"

                "} else {"

                    "status.textContent = "
                    "'OTA failed: ' + text;"

                "}"

            "} catch (error) {"

                "status.textContent = "
                "'Upload error: ' + error;"

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


/* ============================================================
 * OTA Handler
 * ============================================================ */

static esp_err_t ota_handler(httpd_req_t *req)
{
    ESP_LOGI(
        TAG,
        "Starting OTA update"
    );

    ESP_LOGI(
        TAG,
        "Content-Length: %d bytes",
        req->content_len
    );


    /* --------------------------------------------------------
     * Validate Content-Length
     * -------------------------------------------------------- */

    if (req->content_len <= 0) {

        ESP_LOGE(
            TAG,
            "Invalid Content-Length"
        );

        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Invalid firmware size"
        );

        return ESP_FAIL;
    }


    /* --------------------------------------------------------
     * Get next OTA partition
     * -------------------------------------------------------- */

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

    ESP_LOGI(
        TAG,
        "Partition address: 0x%lx",
        (unsigned long)update_partition->address
    );

    ESP_LOGI(
        TAG,
        "Partition size: %lu bytes",
        (unsigned long)update_partition->size
    );


    /* --------------------------------------------------------
     * Check firmware size
     * -------------------------------------------------------- */

    if ((size_t)req->content_len >
        update_partition->size) {

        ESP_LOGE(
            TAG,
            "Firmware too large: %d bytes",
            req->content_len
        );

        httpd_resp_send_err(
            req,
            HTTPD_400_BAD_REQUEST,
            "Firmware is too large"
        );

        return ESP_FAIL;
    }


    /* --------------------------------------------------------
     * Start OTA
     * -------------------------------------------------------- */

    esp_ota_handle_t ota_handle = 0;

    esp_err_t err = esp_ota_begin(
        update_partition,
        req->content_len,
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


    /* --------------------------------------------------------
     * Allocate OTA buffer from heap
     * -------------------------------------------------------- */

    char *buffer =
        malloc(OTA_BUFFER_SIZE);

    if (buffer == NULL) {

        ESP_LOGE(
            TAG,
            "Failed to allocate OTA buffer"
        );

        esp_ota_abort(
            ota_handle
        );

        httpd_resp_send_err(
            req,
            HTTPD_500_INTERNAL_SERVER_ERROR,
            "Out of memory"
        );

        return ESP_ERR_NO_MEM;
    }


    /* --------------------------------------------------------
     * Receive and write firmware
     * -------------------------------------------------------- */

    int total_received = 0;

    bool first_chunk = true;

    while (total_received < req->content_len) {

        int remaining =
            req->content_len - total_received;

        int to_receive =
            remaining > OTA_BUFFER_SIZE
                ? OTA_BUFFER_SIZE
                : remaining;


        int received = httpd_req_recv(
            req,
            buffer,
            to_receive
        );


        /* Timeout is recoverable */

        if (received == HTTPD_SOCK_ERR_TIMEOUT) {

            continue;
        }


        /* Connection error */

        if (received <= 0) {

            ESP_LOGE(
                TAG,
                "Firmware receive failed: %d",
                received
            );

            free(buffer);

            esp_ota_abort(
                ota_handle
            );

            httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "Firmware upload interrupted"
            );

            return ESP_FAIL;
        }


        /* ----------------------------------------------------
         * Validate ESP32 application magic byte
         *
         * ESP32 application images start with 0xE9.
         * ---------------------------------------------------- */

        if (first_chunk) {

            first_chunk = false;

            ESP_LOGI(
                TAG,
                "Firmware magic: 0x%02X",
                buffer[0]
            );

            if (buffer[0] != 0xE9) {

                ESP_LOGE(
                    TAG,
                    "Invalid firmware magic: 0x%02X",
                    buffer[0]
                );

                free(buffer);

                esp_ota_abort(
                    ota_handle
                );

                httpd_resp_send_err(
                    req,
                    HTTPD_400_BAD_REQUEST,
                    "Invalid ESP32 firmware image"
                );

                return ESP_FAIL;
            }
        }


        /* ----------------------------------------------------
         * Write firmware chunk
         * ---------------------------------------------------- */

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

            free(buffer);

            esp_ota_abort(
                ota_handle
            );

            httpd_resp_send_err(
                req,
                HTTPD_500_INTERNAL_SERVER_ERROR,
                "OTA write failed"
            );

            return err;
        }


        total_received += received;

        ESP_LOGI(
            TAG,
            "Received: %d / %d bytes",
            total_received,
            req->content_len
        );
    }


    /* --------------------------------------------------------
     * Free upload buffer
     * -------------------------------------------------------- */

    free(buffer);

    ESP_LOGI(
        TAG,
        "Firmware upload complete"
    );


    /* --------------------------------------------------------
     * Finish OTA
     * -------------------------------------------------------- */

    err = esp_ota_end(
        ota_handle
    );

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


    /* --------------------------------------------------------
     * Set new firmware as boot partition
     * -------------------------------------------------------- */

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


    /* --------------------------------------------------------
     * OTA successful
     * -------------------------------------------------------- */

    ESP_LOGI(
        TAG,
        "OTA successful!"
    );

    ESP_LOGI(
        TAG,
        "Total received: %d bytes",
        total_received
    );


    /* Send response before reboot */

    httpd_resp_set_type(
        req,
        "text/plain"
    );

    httpd_resp_sendstr(
        req,
        "OTA successful. Device will reboot..."
    );


    /*
     * Give the TCP stack/browser time to receive
     * the HTTP response before restarting.
     */

    vTaskDelay(
        pdMS_TO_TICKS(1500)
    );


    ESP_LOGI(
        TAG,
        "Rebooting into new firmware..."
    );

    esp_restart();

    return ESP_OK;
}


/* ============================================================
 * HTTP Server
 * ============================================================ */

static void start_webserver(void)
{
    httpd_config_t config =
        HTTPD_DEFAULT_CONFIG();

    /*
     * OTA handler uses a heap buffer, but increase the
     * HTTP server stack for additional safety.
     */

    config.stack_size = 8192;

    config.max_uri_handlers = 8;


    httpd_handle_t server = NULL;

    ESP_ERROR_CHECK(
        httpd_start(
            &server,
            &config
        )
    );


    /* --------------------------------------------------------
     * GET /
     * -------------------------------------------------------- */

    httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_handler,
        .user_ctx = NULL
    };


    /* --------------------------------------------------------
     * POST /update
     * -------------------------------------------------------- */

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


/* ============================================================
 * Application Entry Point
 * ============================================================ */

void app_main(void)
{
    ESP_LOGI(
        TAG,
        "ESP32-S3 OTA Demo"
    );

    ESP_LOGI(
        TAG,
        "ESP-IDF: %s",
        esp_get_idf_version()
    );


    /* --------------------------------------------------------
     * Initialize NVS
     * -------------------------------------------------------- */

    esp_err_t ret =
        nvs_flash_init();

    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {

        ESP_ERROR_CHECK(
            nvs_flash_erase()
        );

        ret = nvs_flash_init();
    }

    ESP_ERROR_CHECK(ret);


    /* --------------------------------------------------------
     * Initialize WiFi
     * -------------------------------------------------------- */

    wifi_init();


    /*
     * Give WiFi some time to connect and obtain an IP.
     */

    vTaskDelay(
        pdMS_TO_TICKS(3000)
    );


    /* --------------------------------------------------------
     * Start HTTP OTA server
     * -------------------------------------------------------- */

    start_webserver();
}
