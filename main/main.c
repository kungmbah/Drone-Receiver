#include <stdio.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/timer.h"

#include "lwip/sockets.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "ppm_generator.h"

#define WIFI_SSID "ruter"
#define WIFI_PASS "caksa123"

#define PORT 3333

#define PPM_FRAME_LENGTH 22500
#define PPM_PULSE_LENGTH 300
#define PPM_CHANNELS 8
#define DEFAULT_CHANNEL_VALUE 1500
#define OUTPUT_PIN GPIO_NUM_15

uint16_t channelValue[PPM_CHANNELS] = {1500, 1500, 1500, 1500, 1500, 1500, 1500, 1500};
hw_timer_t *timer = NULL;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;


enum ppmState_e
{
    PPM_STATE_IDLE,
    PPM_STATE_PULSE,
    PPM_STATE_FILL,
    PPM_STATE_SYNC
};

static void udp_server_task(void *pvParameters)
{
    char rx_buffer[128];
    char delim[] = "rptymag";
    int addr_family = (int)pvParameters;
    int ip_protocol = 0;
    struct sockaddr_in6 dest_addr;
    int failsafe_count = 0;

    while (1)
    {
        struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
        dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
        dest_addr_ip4->sin_family = AF_INET;
        dest_addr_ip4->sin_port = htons(PORT);
        ip_protocol = IPPROTO_IP;

        int sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0)
        {
            break;
        }

        // Set timeout
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 5000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

        (void)bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
 
        struct sockaddr_storage source_addr; // Large enough for both IPv4 or IPv6
        socklen_t socklen = sizeof(source_addr);

        while (1)
        {
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);

            char *ptr = strtok(rx_buffer, delim);
            int i = 0;
            while (ptr != NULL && len > 0)
            {
                channelValue[i] = atof(ptr); // Convert token to float
                ptr = strtok(NULL, delim);
                failsafe_count = 0;
                i++;
            }

            if (len > 0)
            {
                rx_buffer[len] = 0; // Null-terminate
            }
            
            else if (len <= 0)
            {
                failsafe_count++;
                if (failsafe_count >= 50)
                {
                    printf("FAILSAFE!!!");
                    channelValue[4] = 2000;
                    channelValue[2] = 1000;
                    failsafe_count = 50;
                }
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }

        if (sock != -1)
        {
            shutdown(sock, 0);
            close(sock);
        }
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *event_handler_arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id)
    {
    case WIFI_EVENT_STA_START:
        printf("WiFi connecting WIFI_EVENT_STA_START ... \n");
        break;
    case WIFI_EVENT_STA_CONNECTED:
        printf("WiFi connected WIFI_EVENT_STA_CONNECTED ... \n");
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        printf("WiFi lost connection WIFI_EVENT_STA_DISCONNECTED ... \n");
        esp_wifi_start();
        esp_wifi_connect();
        break;
    case IP_EVENT_STA_GOT_IP:
        printf("WiFi got IP ... \n\n");
        break;
    default:
        break;
    }
}

void wifi_connection()
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_initiation = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&wifi_initiation);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL);

    wifi_config_t wifi_configuration = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS}};
    esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_configuration);
    esp_wifi_set_mode(WIFI_MODE_STA);

    esp_wifi_start();
    esp_wifi_connect();
}

void IRAM_ATTR onPpmTimer()
{
    static uint8_t ppmState = PPM_STATE_IDLE;
    static uint8_t ppmChannel = 0;
    static uint8_t ppmOutput = 0;
    static int usedFrameLength = 0;
    int currentChannelValue;

    portENTER_CRITICAL_ISR(&timerMux);

    if (ppmState == PPM_STATE_IDLE)
    {
        ppmState = PPM_STATE_PULSE;
        ppmChannel = 0;
        usedFrameLength = 0;
        ppmOutput = 0;
    }

    if (ppmState == PPM_STATE_PULSE)
    {
        ppmOutput = 1;
        usedFrameLength += PPM_PULSE_LENGTH;
        ppmState = PPM_STATE_FILL;

        timerAlarmWrite(timer, PPM_PULSE_LENGTH, true);
    }
    else if (ppmState == PPM_STATE_FILL)
    {
        ppmOutput = 0;
        currentChannelValue = channelValue[ppmChannel];

        ppmChannel++;
        ppmState = PPM_STATE_PULSE;

        if (ppmChannel >= PPM_CHANNELS)
        {
            ppmChannel = 0;
            timerAlarmWrite(timer, PPM_FRAME_LENGTH - usedFrameLength, true);
            usedFrameLength = 0;
        }
        else
        {
            usedFrameLength += currentChannelValue - PPM_PULSE_LENGTH;
            timerAlarmWrite(timer, currentChannelValue - PPM_PULSE_LENGTH, true);
        }
    }
    portEXIT_CRITICAL_ISR(&timerMux);
    gpio_set_level(OUTPUT_PIN, ppmOutput);
}

void ppm_start()
{
    timer = ppm_init();
    timerAttachInterrupt(timer, &onPpmTimer, true);
    timerAlarmWrite(timer, 12000, true);
    timerAlarmEnable(timer);    
}

void app_main()
{
    wifi_connection();
    ppm_start();

    vTaskDelay(1000 / portTICK_PERIOD_MS);
    xTaskCreate(udp_server_task, "udp_server", 4096, (void *)AF_INET, 5, NULL);
}