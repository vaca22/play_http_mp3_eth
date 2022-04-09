/* Play an MP3 file from HTTP

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <lwip/sockets.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "audio_element.h"
#include "audio_pipeline.h"
#include "audio_event_iface.h"
#include "audio_common.h"
#include "http_stream.h"
#include "i2s_stream.h"
#include "mp3_decoder.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_peripherals.h"
#include "periph_wifi.h"
#include "board.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"
#include "audio_idf_version.h"

#if (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(4, 1, 0))
#include "esp_netif.h"
#else
#include "tcpip_adapter.h"
#endif

static const char *TAG = "HTTP_MP3_EXAMPLE";
#define WIFI_CONNECTED_BIT							BIT0
EventGroupHandle_t tcp_event_group;					// wifi建立成功信号量
struct sockaddr_in client_addr;

int connect_socket=0;



static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
        case ETHERNET_EVENT_CONNECTED:
            esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
            ESP_LOGI(TAG, "Ethernet Link Up");
            ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                     mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
            break;
        case ETHERNET_EVENT_DISCONNECTED:
            xEventGroupClearBits(tcp_event_group, WIFI_CONNECTED_BIT);
            ESP_LOGI(TAG, "Ethernet Link Down");
            break;
        case ETHERNET_EVENT_START:
            ESP_LOGI(TAG, "Ethernet Started");
            break;
        case ETHERNET_EVENT_STOP:
            ESP_LOGI(TAG, "Ethernet Stopped");
            break;
        default:
            break;
    }
}
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    xEventGroupSetBits(tcp_event_group, WIFI_CONNECTED_BIT);
  //  xTaskCreate(&udp_connect, "udp_connect", 4096, NULL, 5, NULL);
}

#define WEB_SERVER "139.9.206.3"
#define WEB_PORT "8000"
#define WEB_PATH "/"
static const char *REQUEST = "GET " WEB_PATH " HTTP/1.0\r\n"
"Host: "WEB_SERVER":"WEB_PORT"\r\n"
"User-Agent: esp-idf/1.0 esp32\r\n"
"\r\n";

static void http_get_task(void *pvParameters)
{
    const struct addrinfo hints = {
            .ai_family = AF_INET,
            .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    char recv_buf[64];

    while(1) {
        int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);

        if(err != 0 || res == NULL) {
            ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }

        /* Code to print the resolved IP.

           Note: inet_ntoa is non-reentrant, look at ipaddr_ntoa_r for "real" code */
        addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
        ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

        s = socket(res->ai_family, res->ai_socktype, 0);
        if(s < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... allocated socket");

        if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }

        ESP_LOGI(TAG, "... connected");
        freeaddrinfo(res);

        if (write(s, REQUEST, strlen(REQUEST)) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... socket send success");

        struct timeval receiving_timeout;
        receiving_timeout.tv_sec = 5;
        receiving_timeout.tv_usec = 0;
        if (setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &receiving_timeout,
                       sizeof(receiving_timeout)) < 0) {
            ESP_LOGE(TAG, "... failed to set socket receiving timeout");
            close(s);
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            continue;
        }
        ESP_LOGI(TAG, "... set socket receiving timeout success");

        /* Read HTTP response */
        do {
            bzero(recv_buf, sizeof(recv_buf));
            r = read(s, recv_buf, sizeof(recv_buf)-1);
            for(int i = 0; i < r; i++) {
                putchar(recv_buf[i]);
            }
        } while(r > 0);

        ESP_LOGI(TAG, "... done reading from socket. Last read return=%d errno=%d.", r, errno);
        close(s);
        for(int countdown = 10; countdown >= 0; countdown--) {
            ESP_LOGI(TAG, "%d... ", countdown);
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        ESP_LOGI(TAG, "Starting again!");
    }
}

void app_main(void)
{
    tcp_event_group = xEventGroupCreate();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());


    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();

    phy_config.phy_addr = 1;
    phy_config.reset_gpio_num = -1;
    mac_config.smi_mdc_gpio_num = 23;
    mac_config.smi_mdio_gpio_num = 18;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&mac_config);

    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_config);

    esp_eth_config_t config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    ESP_ERROR_CHECK(esp_eth_driver_install(&config, &eth_handle));
    /* attach Ethernet driver to TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));


    // Register user defined event handers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));


    xEventGroupWaitBits(tcp_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    xTaskCreate(&http_get_task, "http_get_task", 4096, NULL, 5, NULL);

//    audio_pipeline_handle_t pipeline;
//    audio_element_handle_t http_stream_reader, i2s_stream_writer, mp3_decoder;
//
//    esp_log_level_set("*", ESP_LOG_WARN);
//    esp_log_level_set(TAG, ESP_LOG_DEBUG);
//
//    ESP_LOGI(TAG, "[ 1 ] Start audio codec chip");
//    audio_board_handle_t board_handle = audio_board_init();
//    audio_hal_ctrl_codec(board_handle->audio_hal, AUDIO_HAL_CODEC_MODE_DECODE, AUDIO_HAL_CTRL_START);
//
//    ESP_LOGI(TAG, "[2.0] Create audio pipeline for playback");
//    audio_pipeline_cfg_t pipeline_cfg = DEFAULT_AUDIO_PIPELINE_CONFIG();
//    pipeline = audio_pipeline_init(&pipeline_cfg);
//    mem_assert(pipeline);
//
//    ESP_LOGI(TAG, "[2.1] Create http stream to read data");
//    http_stream_cfg_t http_cfg = HTTP_STREAM_CFG_DEFAULT();
//    http_stream_reader = http_stream_init(&http_cfg);
//
//    ESP_LOGI(TAG, "[2.2] Create i2s stream to write data to codec chip");
//    i2s_stream_cfg_t i2s_cfg = I2S_STREAM_CFG_DEFAULT();
//    i2s_cfg.type = AUDIO_STREAM_WRITER;
//    i2s_stream_writer = i2s_stream_init(&i2s_cfg);
//
//    ESP_LOGI(TAG, "[2.3] Create mp3 decoder to decode mp3 file");
//    mp3_decoder_cfg_t mp3_cfg = DEFAULT_MP3_DECODER_CONFIG();
//    mp3_decoder = mp3_decoder_init(&mp3_cfg);
//
//    ESP_LOGI(TAG, "[2.4] Register all elements to audio pipeline");
//    audio_pipeline_register(pipeline, http_stream_reader, "http");
//    audio_pipeline_register(pipeline, mp3_decoder,        "mp3");
//    audio_pipeline_register(pipeline, i2s_stream_writer,  "i2s");
//
//    ESP_LOGI(TAG, "[2.5] Link it together http_stream-->mp3_decoder-->i2s_stream-->[codec_chip]");
//    const char *link_tag[3] = {"http", "mp3", "i2s"};
//    audio_pipeline_link(pipeline, &link_tag[0], 3);
//
//    ESP_LOGI(TAG, "[2.6] Set up  uri (http as http_stream, mp3 as mp3 decoder, and default output is i2s)");
//    audio_element_set_uri(http_stream_reader, "http://139.9.206.3:8000/x3.mp3");
//
//
//    ESP_LOGE(TAG, "[ 3 ] Stagagawork");
//
//    ESP_LOGI(TAG, "[ 4 ] Set up  event listener");
//    audio_event_iface_cfg_t evt_cfg = AUDIO_EVENT_IFACE_DEFAULT_CFG();
//    audio_event_iface_handle_t evt = audio_event_iface_init(&evt_cfg);
//
//    ESP_LOGI(TAG, "[4.1] Listening event from all elements of pipeline");
//    audio_pipeline_set_listener(pipeline, evt);
//
//    ESP_LOGI(TAG, "[4.2] Listening event from peripherals");
//
//    ESP_LOGI(TAG, "[ 5 ] Start audio_pipeline");
//    audio_pipeline_run(pipeline);
//
//    while (1) {
//        audio_event_iface_msg_t msg;
//        esp_err_t ret = audio_event_iface_listen(evt, &msg, portMAX_DELAY);
//        if (ret != ESP_OK) {
//            ESP_LOGE(TAG, "[ * ] Event interface error : %d", ret);
//            continue;
//        }
//
//        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT
//            && msg.source == (void *) mp3_decoder
//            && msg.cmd == AEL_MSG_CMD_REPORT_MUSIC_INFO) {
//            audio_element_info_t music_info = {0};
//            audio_element_getinfo(mp3_decoder, &music_info);
//
//            ESP_LOGI(TAG, "[ * ] Receive music info from mp3 decoder, sample_rates=%d, bits=%d, ch=%d",
//                     music_info.sample_rates, music_info.bits, music_info.channels);
//
//            audio_element_setinfo(i2s_stream_writer, &music_info);
//            i2s_stream_set_clk(i2s_stream_writer, music_info.sample_rates, music_info.bits, music_info.channels);
//            continue;
//        }
//
//
//        if (msg.source_type == AUDIO_ELEMENT_TYPE_ELEMENT && msg.source == (void *) i2s_stream_writer
//            && msg.cmd == AEL_MSG_CMD_REPORT_STATUS
//            && (((int)msg.data == AEL_STATUS_STATE_STOPPED) || ((int)msg.data == AEL_STATUS_STATE_FINISHED))) {
//            ESP_LOGW(TAG, "[ * ] Stop event received");
//            break;
//        }
//    }
//
//
//    ESP_LOGI(TAG, "[ 6 ] Stop audio_pipeline");
//    audio_pipeline_stop(pipeline);
//    audio_pipeline_wait_for_stop(pipeline);
//    audio_pipeline_terminate(pipeline);
//
//
//    audio_pipeline_unregister(pipeline, http_stream_reader);
//    audio_pipeline_unregister(pipeline, i2s_stream_writer);
//    audio_pipeline_unregister(pipeline, mp3_decoder);
//
//    audio_pipeline_remove_listener(pipeline);
//
//
//    audio_event_iface_destroy(evt);
//
//
//    audio_pipeline_deinit(pipeline);
//    audio_element_deinit(http_stream_reader);
//    audio_element_deinit(i2s_stream_writer);
//    audio_element_deinit(mp3_decoder);

}
