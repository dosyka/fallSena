/*
 * FallSena DATA-LOGGING firmware (separate from the main firmware).
 *
 * Purpose: record LABELED IMU trials for fall-detection algorithm tuning.
 * The MPU6050 is sampled at 100 Hz at WIDE full-scale ranges (+/-8 g accel,
 * +/-1000 dps gyro) so real impacts do not clip. The button toggles
 * recording; on stop the whole buffered trial is flushed to a host PC over
 * TCP/WiFi as newline-delimited ASCII CSV.
 *
 * State machine (one short button press = toggle):
 *   IDLE      -> short press -> RECORDING  (LED red, single vibration pulse)
 *   RECORDING -> short press -> flush over TCP, IDLE (LED green, double pulse)
 *   IDLE with unsent data -> short press -> RETRY flush (no new trial started)
 *
 * If the RAM buffer fills (FALLSENA_MAX_SAMPLES), recording auto-stops and
 * flushes. If no host is connected at flush time, the data is KEPT buffered
 * and a warning is logged; the next short press retries the flush.
 *
 * WiFi/TCP design: the ESP joins your network as a STA (client) and then runs
 * a TCP SERVER on FALLSENA_TCP_PORT (default 3333). The host PC connects to
 * the ESP's IP (printed over USB serial at boot). On each trial-stop the ESP
 * pushes the trial over the accepted connection.
 *
 * Wire format (must match the Python host EXACTLY):
 *   #START,<trial_id>\n
 *   timestamp_ms,sample_idx,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n   (header)
 *   <t_ms>,<idx>,<ax_g>,<ay_g>,<az_g>,<gx_dps>,<gy_dps>,<gz_dps>\n  (per sample)
 *   #STOP,<sample_count>\n
 *  accel in g (4 decimals), gyro in deg/s (2 decimals), t_ms since trial start
 *  (sample 0 == 0), trial_id == boot-ms at the moment recording started.
 */

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include "mpu6050.h"
#include "rgb_led.h"
#include "vibration.h"
#include "button.h"
#include "wifi_config.h"

#define FALLSENA_DATALOG_VERSION  "1.0.0"

/* ------------------------------------------------------------------ */
/* Sampling configuration                                             */
/* ------------------------------------------------------------------ */

/* IMU sample rate. Make-it-a-#define per spec. */
#define FALLSENA_SAMPLE_RATE_HZ    100
#define FALLSENA_SAMPLE_PERIOD_MS  (1000 / FALLSENA_SAMPLE_RATE_HZ)  /* 10 ms */

/*
 * Max trial length: a static RAM buffer of FALLSENA_MAX_SAMPLES samples.
 * 3000 samples @ 100 Hz == 30 seconds. Each sample is 6 floats + 1 uint32
 * timestamp = 28 bytes, so the buffer is ~84 KB -- comfortably within the
 * ESP32-C6's ~512 KB SRAM. Raise FALLSENA_MAX_SAMPLES for longer trials.
 */
#define FALLSENA_MAX_SAMPLES       3000
#define FALLSENA_MAX_TRIAL_SECONDS (FALLSENA_MAX_SAMPLES / FALLSENA_SAMPLE_RATE_HZ)

/* Vibration feedback durations (ms). */
#define FALLSENA_PULSE_START_MS    120  /* single pulse: recording started   */
#define FALLSENA_PULSE_SENT_MS     80   /* each of a double pulse: trial sent */
#define FALLSENA_PULSE_GAP_MS      80   /* gap between the two "sent" pulses  */

static const char *TAG = "datalog";

/* ------------------------------------------------------------------ */
/* Trial buffer (in RAM)                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t t_ms;     /* ms since trial start (sample 0 == 0) */
    float ax_g, ay_g, az_g;    /* accel in g     */
    float gx_dps, gy_dps, gz_dps;  /* gyro in deg/s */
} datalog_sample_t;

static datalog_sample_t s_buffer[FALLSENA_MAX_SAMPLES];
static int      s_count;            /* samples currently buffered            */
static bool     s_recording;        /* true while actively buffering         */
static bool     s_have_unsent;      /* a completed trial is waiting to flush  */
static uint32_t s_trial_id;         /* boot-ms at the start of this trial     */
static int64_t  s_trial_start_us;   /* esp_timer time at trial start          */

/* ------------------------------------------------------------------ */
/* WiFi state                                                         */
/* ------------------------------------------------------------------ */

#define WIFI_CONNECTED_BIT  BIT0
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (s_wifi_retry < FALLSENA_WIFI_MAX_RETRY) {
            s_wifi_retry++;
            esp_wifi_connect();
            ESP_LOGW(TAG, "WiFi disconnected, retry %d/%d",
                     s_wifi_retry, FALLSENA_WIFI_MAX_RETRY);
        } else {
            /* Keep retrying slowly in the background; recording still works. */
            esp_wifi_connect();
            ESP_LOGW(TAG, "WiFi: AP unreachable, still retrying in background");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_wifi_retry = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "============================================");
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Host: connect a TCP client to " IPSTR ":%d",
                 IP2STR(&event->ip_info.ip), FALLSENA_TCP_PORT);
        ESP_LOGI(TAG, "============================================");
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi STA started, joining SSID \"%s\"", WIFI_SSID);
}

/* ------------------------------------------------------------------ */
/* TCP server: accept a host connection in the background             */
/* ------------------------------------------------------------------ */

static volatile int s_client_sock = -1;     /* accepted host socket, or -1   */
static SemaphoreHandle_t s_sock_mutex;       /* guards s_client_sock changes  */

static void tcp_server_task(void *arg)
{
    /* Wait until WiFi is up before binding. */
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Unable to create listen socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in dest_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(FALLSENA_TCP_PORT),
    };
    if (bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr)) != 0) {
        ESP_LOGE(TAG, "Socket bind failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "Socket listen failed: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "TCP server listening on port %d", FALLSENA_TCP_PORT);

    while (1) {
        /* Only accept a new client while none is connected. */
        if (s_client_sock >= 0) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr,
                          &addr_len);
        if (sock < 0) {
            ESP_LOGW(TAG, "accept() failed: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        char addr_str[16];
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str));
        xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
        s_client_sock = sock;
        xSemaphoreGive(s_sock_mutex);
        ESP_LOGI(TAG, "Host connected from %s", addr_str);
    }
}

/* Send the whole NUL-terminated string; returns true on success. */
static bool socket_send_all(int sock, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = send(sock, data + sent, len - sent, 0);
        if (n < 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

/*
 * Flush the buffered trial to the connected host over TCP.
 * Returns true if the trial was sent (buffer may then be cleared), false if
 * there was no host connection or a send failed (buffer should be KEPT).
 */
static bool flush_trial(void)
{
    xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
    int sock = s_client_sock;
    xSemaphoreGive(s_sock_mutex);

    if (sock < 0) {
        ESP_LOGW(TAG, "No host connected; keeping %d samples buffered "
                      "(short-press again to retry)", s_count);
        return false;
    }

    char line[160];
    int n;
    bool ok = true;

    /* Control: trial start. */
    n = snprintf(line, sizeof(line), "#START,%u\n", (unsigned)s_trial_id);
    ok = ok && socket_send_all(sock, line, (size_t)n);

    /* CSV header (exact). */
    if (ok) {
        const char *hdr =
            "timestamp_ms,sample_idx,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps\n";
        ok = socket_send_all(sock, hdr, strlen(hdr));
    }

    /* Data lines. */
    for (int i = 0; ok && i < s_count; i++) {
        const datalog_sample_t *s = &s_buffer[i];
        n = snprintf(line, sizeof(line),
                     "%u,%d,%.4f,%.4f,%.4f,%.2f,%.2f,%.2f\n",
                     (unsigned)s->t_ms, i,
                     s->ax_g, s->ay_g, s->az_g,
                     s->gx_dps, s->gy_dps, s->gz_dps);
        ok = socket_send_all(sock, line, (size_t)n);
    }

    /* Control: trial stop. */
    if (ok) {
        n = snprintf(line, sizeof(line), "#STOP,%d\n", s_count);
        ok = socket_send_all(sock, line, (size_t)n);
    }

    if (!ok) {
        ESP_LOGW(TAG, "Send failed mid-trial; dropping host, keeping buffer");
        xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
        if (s_client_sock == sock) {
            close(s_client_sock);
            s_client_sock = -1;
        }
        xSemaphoreGive(s_sock_mutex);
        return false;
    }

    ESP_LOGI(TAG, "Trial %u flushed (%d samples) to host",
             (unsigned)s_trial_id, s_count);
    return true;
}

/* Double vibration pulse to confirm a trial was sent. */
static void vibration_double_pulse(void)
{
    vibration_pulse(FALLSENA_PULSE_SENT_MS);
    vTaskDelay(pdMS_TO_TICKS(FALLSENA_PULSE_GAP_MS));
    vibration_pulse(FALLSENA_PULSE_SENT_MS);
}

/* ------------------------------------------------------------------ */
/* Recording control                                                  */
/* ------------------------------------------------------------------ */

static void start_recording(void)
{
    s_count = 0;
    s_have_unsent = false;
    s_trial_start_us = esp_timer_get_time();
    s_trial_id = (uint32_t)(s_trial_start_us / 1000);  /* boot-ms */
    s_recording = true;

    rgb_led_set_color(RGB_LED_RED);
    vibration_pulse(FALLSENA_PULSE_START_MS);  /* single pulse: started */
    ESP_LOGI(TAG, "RECORDING started (trial_id=%u, max %d s)",
             (unsigned)s_trial_id, FALLSENA_MAX_TRIAL_SECONDS);
}

/* Stop recording and attempt to flush. Updates idle state/LED/vibration. */
static void stop_and_flush(void)
{
    s_recording = false;
    ESP_LOGI(TAG, "RECORDING stopped (%d samples), flushing...", s_count);

    bool sent = flush_trial();
    if (sent) {
        s_have_unsent = false;
        rgb_led_set_color(RGB_LED_GREEN);  /* idle */
        vibration_double_pulse();          /* confirm sent */
    } else {
        /* Keep the buffer for a later retry; amber-ish cue via blue. */
        s_have_unsent = true;
        rgb_led_set_color(RGB_LED_BLUE);   /* idle, data pending */
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

void app_main(void)
{
    ESP_LOGI(TAG, "FallSena DATA-LOGGER booting (v%s)", FALLSENA_DATALOG_VERSION);
    ESP_LOGI(TAG, "Config: %d Hz, accel +/-8g, gyro +/-1000dps, "
                  "max trial %d s (%d samples)",
             FALLSENA_SAMPLE_RATE_HZ, FALLSENA_MAX_TRIAL_SECONDS,
             FALLSENA_MAX_SAMPLES);

    /* NVS is required by the WiFi stack. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_err);
    }

    s_sock_mutex = xSemaphoreCreateMutex();

    /* Initialize the IMU and other I2C/GPIO peripherals BEFORE powering up the
     * WiFi radio. Bringing WiFi up first causes current bursts / I2C bus
     * disturbance that make the IMU's first I2C write NACK. */

    /* IMU is the core function: if it fails, idle instead of streaming junk. */
    esp_err_t err = mpu6050_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 init failed (%s); not recording.",
                 esp_err_to_name(err));
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /* User-facing peripherals (best-effort). */
    if (rgb_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed; continuing without it");
    }
    if (vibration_init() != ESP_OK) {
        ESP_LOGW(TAG, "vibration init failed; continuing without it");
    }
    if (button_init() != ESP_OK) {
        ESP_LOGW(TAG, "button init failed; continuing without it");
    }

    /* Now that the I2C peripherals are up, bring up WiFi + TCP server.
     * The IP is logged slightly later in boot as a result. */
    wifi_init_sta();
    xTaskCreate(tcp_server_task, "tcp_server", 4096, NULL, 5, NULL);

    rgb_led_set_color(RGB_LED_GREEN);  /* idle */
    ESP_LOGI(TAG, "Ready. Short-press the button to start/stop a trial.");

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        /* Button toggles recording (short press). LONG press is unused. */
        if (button_get_event() == BUTTON_EVENT_SHORT) {
            if (s_recording) {
                stop_and_flush();
            } else if (s_have_unsent) {
                /* Don't start a new trial over unsent data: retry the flush. */
                ESP_LOGI(TAG, "Retrying flush of buffered trial...");
                stop_and_flush();  /* s_recording already false; just flushes */
            } else {
                start_recording();
            }
            /* Re-align the cadence after any blocking vibration feedback. */
            last_wake = xTaskGetTickCount();
        }

        if (s_recording) {
            mpu6050_scaled_data_t scaled;
            if (mpu6050_read(NULL, &scaled) == ESP_OK) {
                datalog_sample_t *s = &s_buffer[s_count];
                s->t_ms = (uint32_t)((esp_timer_get_time() - s_trial_start_us)
                                     / 1000);
                s->ax_g = scaled.accel_x;
                s->ay_g = scaled.accel_y;
                s->az_g = scaled.accel_z;
                s->gx_dps = scaled.gyro_x;
                s->gy_dps = scaled.gyro_y;
                s->gz_dps = scaled.gyro_z;
                s_count++;

                if (s_count >= FALLSENA_MAX_SAMPLES) {
                    ESP_LOGW(TAG, "Buffer full (%d samples); auto-stopping",
                             s_count);
                    stop_and_flush();
                    last_wake = xTaskGetTickCount();
                }
            } else {
                ESP_LOGW(TAG, "IMU read failed during recording");
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FALLSENA_SAMPLE_PERIOD_MS));
    }
}
