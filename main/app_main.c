#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "protocol_examples_common.h"

#include "driver/gpio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "esp_log.h"
#include "mqtt_client.h"

#include "ds18b20.h"

// Set your local broker URI
#define BROKER_URI "mqtts://192.168.0.240:8883"

#define POWER1 GPIO_NUM_16
#define POWER2 GPIO_NUM_17
#define POWER3 GPIO_NUM_18
#define POWER4 GPIO_NUM_19
#define TEMP_BUS GPIO_NUM_5
#define PUBLISH_DELAY_MS 30 * 1000

static const char *TAG    = "MQTTS_EXAMPLE";
esp_mqtt_client_handle_t client = NULL;

// Publish Topics
static const char *LWT_TOPIC       = "tele/ambientNode/LWT";
static const char *LWT_MESSAGE     = "Offline";
static const char *STATE_TOPIC     = "tele/ambientNode/STATE";
static const char *STATE_RESPONSE  = "{\"Time\": \"%s\",\"POWER1\": \"%s\",\"POWER2\": \"%s\",\"POWER3\": \"%s\",\"POWER4\": \"%s\"}";
char state_response[100];
static const char *SENSOR_TOPIC    = "tele/ambientNode/SENSOR";
static const char *SENSOR_RESPONSE = "{\"Time\": \"%s\",\"DS18B20-1\": {\"Id\": \"%s\",\"Temperature\": %0.1f},\"DS18B20-2\": {\"Id\": \"%s\",\"Temperature\": %0.1f},\"TempUnit\": \"C\"}"; 
char sensor_response[200];

// Subscription Topics
static const char *CMND1  = "cmnd/ambientNode/POWER1";
static const char *CMND2  = "cmnd/ambientNode/POWER2";
static const char *CMND3  = "cmnd/ambientNode/POWER3";
static const char *CMND4  = "cmnd/ambientNode/POWER4";

bool power1Shadow = false;
bool power2Shadow = false;
bool power3Shadow = false;
bool power4Shadow = false;
bool isMQTTConnected = false;

DeviceAddress tempSensors[2];
char sId0[18];
char sId1[18];

time_t now = 0;
struct tm timeinfo = { 0 };
char strftime_buf[20];

extern const uint8_t client_cert_pem_start[] asm("_binary_client_crt_start");
extern const uint8_t client_cert_pem_end[] asm("_binary_client_crt_end");
extern const uint8_t client_key_pem_start[] asm("_binary_client_key_start");
extern const uint8_t client_key_pem_end[] asm("_binary_client_key_end");
extern const uint8_t server_cert_pem_start[] asm("_binary_broker_CA_crt_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_broker_CA_crt_end");

static void mqtt_event_data_handler(esp_mqtt_client_handle_t client, esp_mqtt_event_handle_t event);
static void obtain_time(void);
static void initialize_sntp(void);

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%d", base, event_id);
    esp_mqtt_event_handle_t event = event_data;
    esp_mqtt_client_handle_t client = event->client;
    int msg_id;
    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        isMQTTConnected = true;
        msg_id = esp_mqtt_client_subscribe(client, CMND1, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        msg_id = esp_mqtt_client_subscribe(client, CMND2, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        msg_id = esp_mqtt_client_subscribe(client, CMND3, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        msg_id = esp_mqtt_client_subscribe(client, CMND4, 0);
        ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);
        msg_id = esp_mqtt_client_publish(client, LWT_TOPIC, "Online", 0, 0, 0);
        ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        isMQTTConnected = false;
        break;

    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        mqtt_event_data_handler(client,event);
        break;
    
    case MQTT_EVENT_ERROR:
        ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno",  event->error_handle->esp_transport_sock_errno);
            ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
        }
        break;
    default:
        ESP_LOGI(TAG, "Other event id:%d", event->event_id);
        break;
    }
}

static void mqtt_event_data_handler(esp_mqtt_client_handle_t client, esp_mqtt_event_handle_t event){
    
    printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
    printf("DATA=%.*s\r\n", event->data_len, event->data);
    
    if(strncmp(event->topic, CMND1,event->topic_len) == 0) {
        if(strncmp(event->data, "ON",event->data_len) == 0) {
            gpio_set_level(POWER1, 1);
            power1Shadow = true;
        }
        else {
            gpio_set_level(POWER1, 0);
            power1Shadow = false;
        }
    }
    if(strncmp(event->topic, CMND2,event->topic_len) == 0) {
        if(strncmp(event->data, "ON",event->data_len) == 0) {
            gpio_set_level(POWER2, 1);
            power2Shadow = true;
        }
        else {
            gpio_set_level(POWER2, 0);
            power2Shadow = false;
        }
    }
    if(strncmp(event->topic, CMND3,event->topic_len) == 0) {
        if(strncmp(event->data, "ON",event->data_len) == 0) {
            gpio_set_level(POWER3, 1);
            power3Shadow = true;
        }
        else {
            gpio_set_level(POWER3, 0);
            power3Shadow = false;
        }
    }
    if(strncmp(event->topic, CMND4,event->topic_len) == 0){
        if(strncmp(event->data, "ON",event->data_len) == 0){
            gpio_set_level(POWER4, 1);
            power4Shadow = true;
        }
        else {
            gpio_set_level(POWER4, 0);
            power4Shadow = false;
        }
    }
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    sprintf(state_response,STATE_RESPONSE, (char *)strftime_buf, power1Shadow?"ON":"OFF", power2Shadow?"ON":"OFF", power3Shadow?"ON":"OFF", power4Shadow?"ON":"OFF");
    printf(state_response);printf("\r\n");
    int msg_id = esp_mqtt_client_publish(client, STATE_TOPIC, state_response, 0, 0, 0);
    ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
}

static void mqtt_publisher_handler(void* arg){
    float tempId0, tempId1;
    while (true) {
        vTaskDelay(PUBLISH_DELAY_MS / portTICK_PERIOD_MS);
        if(isMQTTConnected) {
            
            do { // busca lecturas "sanas", suele aparecer el valor -196.6°C.
               ds18b20_requestTemperatures();
               tempId0 = ds18b20_getTempC((DeviceAddress *)tempSensors[0]);
               tempId1 = ds18b20_getTempC((DeviceAddress *)tempSensors[1]);
            } while (tempId0 <=0 || tempId1 <=0);
            time(&now);
            localtime_r(&now, &timeinfo);
            strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
            sprintf(sensor_response, SENSOR_RESPONSE, (char*) strftime_buf, sId0, tempId0, sId1, tempId1);
            printf(sensor_response);printf("\r\n");
            int msg_id = esp_mqtt_client_publish(client, SENSOR_TOPIC, sensor_response, 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
        }
    }         
}

static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        .uri = BROKER_URI,
        .client_cert_pem = (const char *)client_cert_pem_start,
        .client_key_pem = (const char *)client_key_pem_start,
        .cert_pem = (const char *)server_cert_pem_start,
        .lwt_topic = LWT_TOPIC,
        .lwt_msg = LWT_MESSAGE,
        .lwt_msg_len = strlen(LWT_MESSAGE),
    };

    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    client = esp_mqtt_client_init(&mqtt_cfg);
    /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}

void getTempAddresses(DeviceAddress *tempSensorAddresses) {
	unsigned int numberFound = 0;
	reset_search();
	// search for 2 addresses on the oneWire protocol
	while (search(tempSensorAddresses[numberFound],true)) {
		numberFound++;
		if (numberFound == 2) break;
	}
	// if 2 addresses aren't found then flash the LED rapidly
	while (numberFound != 2) {
		numberFound = 0;
		// search in the loop for the temp sensors as they may hook them up
		reset_search();
		while (search(tempSensorAddresses[numberFound],true)) {
			numberFound++;
			if (numberFound == 2) break;
		}
	}
}

void app_main(void)
{
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT_BASE", ESP_LOG_VERBOSE);
    esp_log_level_set("TRANSPORT", ESP_LOG_VERBOSE);
    esp_log_level_set("OUTBOX", ESP_LOG_VERBOSE);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    gpio_set_direction(POWER1, GPIO_MODE_OUTPUT);  
    gpio_set_direction(POWER2, GPIO_MODE_OUTPUT);  
    gpio_set_direction(POWER3, GPIO_MODE_OUTPUT);  
    gpio_set_direction(POWER4, GPIO_MODE_OUTPUT);  

	ds18b20_init(TEMP_BUS);
	getTempAddresses(tempSensors);
	ds18b20_setResolution(tempSensors,2,10);

	sprintf(sId0,"%02x%02x%02x%02x%02x%02x%02x%02x", tempSensors[0][0],tempSensors[0][1],tempSensors[0][2],tempSensors[0][3],tempSensors[0][4],tempSensors[0][5],tempSensors[0][6],tempSensors[0][7]);
	sprintf(sId1,"%02x%02x%02x%02x%02x%02x%02x%02x", tempSensors[1][0],tempSensors[1][1],tempSensors[1][2],tempSensors[1][3],tempSensors[1][4],tempSensors[1][5],tempSensors[1][6],tempSensors[1][7]);
    printf("[APP] Sensor 1: %s\r\n", sId0);
    printf("[APP] Sensor 2: %s\r\n", sId1);
    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());

    mqtt_app_start();

    xTaskCreate(mqtt_publisher_handler, "mqtt_publisher_handler", 2048, NULL, 10, NULL);

    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    if (timeinfo.tm_year < (2016 - 1900)) {
        ESP_LOGI(TAG, "Time is not set yet. Connect getting time over NTP.");
        obtain_time();
        time(&now);
     }
}


static void obtain_time(void)
{
    initialize_sntp();

    int retry = 0;
    const int retry_count = 15;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
    time(&now);
    localtime_r(&now, &timeinfo);
   // setenv("TZ", "UTC-3", 1);
   // tzset();
   // localtime_r(&now, &timeinfo); "2022-11-05T19:03:53"
    strftime(strftime_buf, sizeof(strftime_buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "The current date/time is: %s", strftime_buf);
}

static void initialize_sntp(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);

    sntp_setservername(0, "pool.ntp.org");

    sntp_init();

    ESP_LOGI(TAG, "List of configured NTP servers:");

    for (uint8_t i = 0; i < SNTP_MAX_SERVERS; ++i){
        if (sntp_getservername(i)){
            ESP_LOGI(TAG, "server %d: %s", i, sntp_getservername(i));
        } else {
            // we have either IPv4 or IPv6 address, let's print it
            char buff[48];
            ip_addr_t const *ip = sntp_getserver(i);
            if (ipaddr_ntoa_r(ip, buff, 48) != NULL)
                ESP_LOGI(TAG, "server %d: %s", i, buff);
        }
    }
}