#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_timer.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "mqtt_client.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#include "dht11.h"
#include "json_generator.h"

// pin gpio
#define LED1 2
#define LED2 15
#define B4  4
#define B0  0
#define DHT11_PINOUT 5

#define BUF_SIZE (1024)

// wifi information ------------------------------------
#define EXAMPLE_ESP_WIFI_SSID      "ThaoThao"
#define EXAMPLE_ESP_WIFI_PASS      "mothaiba123"
#define EXAMPLE_ESP_MAXIMUM_RETRY  5

// tag for logging
static const char *TAG_MQTT = "MQTT";
static const char *TAG_WIFI = "WIFI STATION";
static const char *TAG_ITTR = "INTERRUPT";
static const char *TAG_BLINK = "BLINK LED";

// declare queue
static QueueHandle_t xQueueButton;

// declare functions
static void mqtt_app_start(void);

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static int s_retry_num = 0;
// end wifi info -----------------------------------------

// handle wifi event: connected, fail...  
static void event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG_WIFI, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG_WIFI,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG_WIFI, "got ip:" IPSTR, IP2STR(&event->ip_info.ip)); // in ra địa chỉ ip
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

// config wifi station
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
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            /* Setting a password implies station will connect to all security modes including WEP/WPA.
             * However these modes are deprecated and not advisable to be used. Incase your Access point
             * doesn't support WPA2, these mode can be enabled by commenting below line */
	     .threshold.authmode = WIFI_AUTH_WPA2_PSK,

            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG_WIFI, "wifi_init_sta finished.");

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
        ESP_LOGI(TAG_WIFI, "connected to ap SSID:%s password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG_WIFI, "Failed to connect to SSID:%s, password:%s",
                 EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS);
    } else {
        ESP_LOGE(TAG_WIFI, "UNEXPECTED EVENT");
    }

    /* The event will not be processed after unregister */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
    vEventGroupDelete(s_wifi_event_group);

    vTaskDelay(1000/portTICK_PERIOD_MS);
    mqtt_app_start();
}

//---------------------------------------------------------------------------------------
/* MQTT over TCP
    Broker: mqtt://core-mosquitto:1883
    user: homeassistant
    password: EeBai0iekeunai7quoh5ebei9aighoojo1woo0iocee2oi9OhquahmoibeDi3iez
*/
esp_mqtt_client_handle_t client;

static const char *msg_connecting = "Hello, Connected to Broker";

static const char *topic_connecting = "home/test/connecting";
static const char *topic_pub_sub_led1 = "home/test/switch1/esp";
static const char *topic_pub_sub_led2 = "home/test/switch2/esp";
static const char *topic_pub_dht11 = "home/test/sensor/dht11";

bool stt_led1 = false, stt_led2 = false;

static esp_err_t mqtt_event_handler_cb(esp_mqtt_event_handle_t event)
{
    client = event->client;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG_MQTT, "MQTT_EVENT_CONNECTED");
            esp_mqtt_client_subscribe(client, topic_pub_sub_led1, 0);
            esp_mqtt_client_subscribe(client, topic_pub_sub_led2, 0);
            esp_mqtt_client_publish(client, topic_connecting, msg_connecting, 5, 0, 0);
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG_MQTT, "MQTT_EVENT_SUBSCRIBED");
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG_MQTT, "MQTT_EVENT_DATA");
            printf("TOPIC = %.*s, ", event->topic_len, event->topic);
            printf("DATA = %.*s\r\n", event->data_len, event->data);
            printf("len = %d", event->topic_len);
            char * topic;
            char * data;
            if (strstr(event->topic, "home/test/switch1/esp") != NULL) {
                topic = topic_sub_led1;
            }
            else if (strstr(event->topic, "home/test/switch2/esp") != NULL) {
                topic = topic_sub_led2;
            }
            else topic = "";

            if (strstr(event->data, "ON") != NULL) {
                data = "ON";
            }
            else if (strstr(event->data, "OFF") != NULL) {
                data = "OFF";
            }
            else data = "";

            printf("\ntopic = %s, data = %s\n", topic, data);
            if (strcmp(topic, topic_sub_led1) == 0) {
                if (strcmp(data, "ON") == 0) {
                    gpio_set_level(LED1, 1);
                    stt_led1 = true;
                }
                else if (strcmp(data, "OFF") == 0) {
                    gpio_set_level(LED1, 0);
                    stt_led1 = false;
                }
            }
            else if (strcmp(topic, topic_sub_led2) == 0) {
                if (strcmp(data, "ON") == 0) {
                    gpio_set_level(LED2, 1);
                    stt_led2 = true;
                }
                else if (strcmp(data, "OFF") == 0) {
                    gpio_set_level(LED2, 0);
                    stt_led2 = false;
                }
            }
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG_MQTT, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(TAG_MQTT, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    ESP_LOGD(TAG_MQTT, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    mqtt_event_handler_cb(event_data);
}

static void mqtt_app_start(void)
{
    // CONFIG_BROKER_URL
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = "mqtt://192.168.0.100:1883",
        .username = "homeassistant",
        .password = "EeBai0iekeunai7quoh5ebei9aighoojo1woo0iocee2oi9OhquahmoibeDi3iez",
    };
    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, client);
    esp_mqtt_client_start(client);
}

typedef struct {
    char buf[256];
    size_t offset;
} json_gen_test_result_t;

typedef struct {
    int temperature, humidity;
} message_t;

static void flush_str(char *buf, void *priv) {
    json_gen_test_result_t *result = (json_gen_test_result_t *)priv;
    if (result) {
        if (strlen(buf) > sizeof(result->buf) - result->offset) {
            printf("Result Buffer too small\r\n");
            return;
        }
        memcpy(result->buf + result->offset, buf, strlen(buf));
        result->offset += strlen(buf);
    }
}

static void json_gen_perform_test(json_gen_test_result_t *result, message_t msg) {
	char buf[20];
    memset(result, 0, sizeof(json_gen_test_result_t));
	json_gen_str_t jstr;
	json_gen_str_start(&jstr, buf, sizeof(buf), flush_str, result);
	json_gen_start_object(&jstr);
    json_gen_obj_set_float(&jstr, "temperature", msg.temperature);
    json_gen_obj_set_float(&jstr, "humidity", msg.humidity);
	json_gen_end_object(&jstr);
	json_gen_str_end(&jstr);
}

void IRAM_ATTR gpio_input_handler(void* arg)  {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(xQueueButton, &gpio_num, NULL);
}

void init_gpio_input(gpio_num_t gpio_num) {
    gpio_config_t io_conf;
    io_conf.pin_bit_mask = 1ull << gpio_num;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    io_conf.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(gpio_num, gpio_input_handler, (void*) gpio_num);
}

void init_gpio_output(gpio_num_t gpio_num) {
    gpio_config_t io_conf;
    io_conf.pin_bit_mask = 1ull << gpio_num;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = 0;
    io_conf.pull_down_en = 0;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
}

// Tasks----------------------------------------------------------------

void handle_intrr(void * pvParameters) {
    uint32_t io_num;
    uint64_t time_db_on = 0, time_db_off = 0;

    while(1)
    {
        if(xQueueReceive(xQueueButton, &io_num, portMAX_DELAY))
        {
			if(esp_timer_get_time()/1000 - time_db_on > 250)
			{
				if(io_num == B4 && gpio_get_level(io_num) == 0)
				{
                    time_db_on = esp_timer_get_time()/1000;
                    stt_led1 = !stt_led1;
                    gpio_set_level(LED1, stt_led1);
                    char *msg = "";
                    msg = (stt_led1 == true) ? "ON":"OFF";
                    esp_mqtt_client_publish(client, topic_pub_sub_led1, msg, strlen(msg), 0, 0);
				}
			}
			if(esp_timer_get_time()/1000 - time_db_off > 250)
			{
				if(io_num == B0 && gpio_get_level(io_num) == 0)
				{
                    time_db_off = esp_timer_get_time()/1000;
                    stt_led2 = !stt_led2;
                    gpio_set_level(LED2, stt_led2);
                    char *msg = "";
                    msg = (stt_led2 == true) ? "ON":"OFF";
                    esp_mqtt_client_publish(client, topic_pub_sub_led2, msg, strlen(msg), 0, 0);
				}
			}
       }
    }
}

void pub_task(void *para) {
    struct dht11_reading dht11;
    for (;;) {
        dht11 = DHT11_read();
        float temp = dht11.temperature;
        float humi = dht11.humidity;
        message_t msg;
        msg.temperature = temp;
        msg.humidity = humi;
        json_gen_test_result_t result;
        json_gen_perform_test(&result, msg);
        esp_mqtt_client_publish(client, topic_pub_dht11, result.buf, strlen(result.buf), 0, 0);
        vTaskDelay(1000/portTICK_PERIOD_MS);
    }
}

void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init(); // include wifi's physical configuration 
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }

    esp_log_level_set(TAG_BLINK, ESP_LOG_VERBOSE);
    esp_log_level_set(TAG_ITTR, ESP_LOG_VERBOSE);
    esp_log_level_set(TAG_MQTT, ESP_LOG_VERBOSE);
    esp_log_level_set(TAG_WIFI, ESP_LOG_VERBOSE);
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_TCP", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_SSL", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    ESP_LOGI(TAG_WIFI, "ESP_WIFI_MODE_STA");

    wifi_init_sta();
    
    vTaskDelay(1000/portTICK_PERIOD_MS);

    init_gpio_output(LED1);
    init_gpio_output(LED2);
    init_gpio_input(B0);
    init_gpio_input(B4);
    DHT11_init(DHT11_PINOUT);

    xQueueButton = xQueueCreate(1, sizeof(uint32_t));

    xTaskCreate(handle_intrr, "handle interrupt task", 4096, NULL, 4, NULL);
    xTaskCreate(pub_task, "publish dht11", 4096, NULL, 4, NULL);

}
