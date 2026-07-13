#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/random/random.h>
#include <string.h>

#include "mqtt_ctrl.h"

LOG_MODULE_REGISTER(mqtt_ctrl, LOG_LEVEL_INF);

/* ── Broker Configuration ────────────────────────────────────────────────── */
#define MQTT_BROKER_HOSTNAME "broker.hivemq.com"
#define MQTT_BROKER_PORT     8883
#define MQTT_TOPIC           "device/ota/cmd"

/* ── MQTT Client Data ────────────────────────────────────────────────────── */
#define MQTT_BUFFER_SIZE  128
static uint8_t rx_buffer[MQTT_BUFFER_SIZE];
static uint8_t tx_buffer[MQTT_BUFFER_SIZE];
static uint8_t payload_buf[MQTT_BUFFER_SIZE];

static struct mqtt_client client_ctx;
static struct sockaddr_storage broker_addr;
static struct mqtt_utf8 client_id;

#define MQTT_THREAD_STACK_SIZE 4096
#define MQTT_THREAD_PRIORITY   8
K_THREAD_STACK_DEFINE(mqtt_thread_stack, MQTT_THREAD_STACK_SIZE);
static struct k_thread mqtt_thread_data;

static bool connected = false;

/* External OTA trigger function defined in main.c */
extern void trigger_ota_update(void);

/* ── DNS Resolution ──────────────────────────────────────────────────────── */
static int resolve_broker(void)
{
    struct zsock_addrinfo *res;
    struct zsock_addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };

    LOG_INF("Resolving %s...", MQTT_BROKER_HOSTNAME);

    int ret = zsock_getaddrinfo(MQTT_BROKER_HOSTNAME, NULL, &hints, &res);
    if (ret != 0) {
        LOG_ERR("getaddrinfo failed: %d", ret);
        return ret;
    }

    struct sockaddr_in *addr4 = (struct sockaddr_in *)&broker_addr;
    addr4->sin_family = AF_INET;
    addr4->sin_port = htons(MQTT_BROKER_PORT);
    addr4->sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;

    char ipv4_str[INET_ADDRSTRLEN];
    zsock_inet_ntop(AF_INET, &addr4->sin_addr, ipv4_str, sizeof(ipv4_str));
    LOG_INF("Resolved broker to %s", ipv4_str);

    zsock_freeaddrinfo(res);
    return 0;
}

/* ── MQTT Callbacks ──────────────────────────────────────────────────────── */
static void mqtt_evt_handler(struct mqtt_client *const client,
                             const struct mqtt_evt *evt)
{
    int ret;

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        if (evt->result != 0) {
            LOG_ERR("MQTT connect failed %d", evt->result);
            break;
        }
        connected = true;
        LOG_INF("MQTT client connected!");

        /* Subscribe to OTA topic */
        struct mqtt_topic subscribe_topic = {
            .topic = {
                .utf8 = (uint8_t *)MQTT_TOPIC,
                .size = strlen(MQTT_TOPIC)
            },
            .qos = MQTT_QOS_0_AT_MOST_ONCE
        };
        const struct mqtt_subscription_list sub_list = {
            .list = &subscribe_topic,
            .list_count = 1,
            .message_id = sys_rand32_get()
        };
        ret = mqtt_subscribe(client, &sub_list);
        if (ret != 0) {
            LOG_ERR("Failed to subscribe to topic: %d", ret);
        } else {
            LOG_INF("Subscribed to topic: %s", MQTT_TOPIC);
        }
        break;

    case MQTT_EVT_DISCONNECT:
        LOG_INF("MQTT client disconnected %d", evt->result);
        connected = false;
        break;

    case MQTT_EVT_PUBLISH:
        LOG_INF("MQTT message received!");
        const struct mqtt_publish_param *pub = &evt->param.publish;
        
        size_t len = pub->message.payload.len;
        if (len < sizeof(payload_buf) - 1) {
            /* Read payload */
            ret = mqtt_read_publish_payload(client, payload_buf, len);
            if (ret >= 0) {
                payload_buf[len] = '\0';
                LOG_INF("Topic: %.*s, Payload: %s", 
                        pub->message.topic.topic.size,
                        pub->message.topic.topic.utf8,
                        payload_buf);

                /* Check for OTA trigger command */
                if (strcmp((char *)payload_buf, "git") == 0) {
                    LOG_INF("Received 'git' command. Triggering OTA update!");
                    trigger_ota_update();
                }
            } else {
                LOG_ERR("Failed to read payload: %d", ret);
            }
        } else {
            LOG_WRN("Payload too large, dropping.");
            /* Drop message */
            mqtt_read_publish_payload(client, payload_buf, sizeof(payload_buf));
        }

        /* Send ACK if QoS > 0 */
        if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            struct mqtt_puback_param puback = {
                .message_id = pub->message_id
            };
            mqtt_publish_qos1_ack(client, &puback);
        }
        break;

    case MQTT_EVT_SUBACK:
        LOG_INF("SUBACK packet id: %u", evt->param.suback.message_id);
        break;

    default:
        LOG_INF("Unhandled MQTT event %d", evt->type);
        break;
    }
}

/* ── MQTT Client Setup and Loop ──────────────────────────────────────────── */
static void mqtt_thread_fn(void *arg1, void *arg2, void *arg3)
{
    ARG_UNUSED(arg1);
    ARG_UNUSED(arg2);
    ARG_UNUSED(arg3);

    int ret;
    char client_id_str[32];
    snprintf(client_id_str, sizeof(client_id_str), "ota_client_%08x", sys_rand32_get());

    client_id.utf8 = (uint8_t *)client_id_str;
    client_id.size = strlen(client_id_str);

    while (true) {
        if (!connected) {
            ret = resolve_broker();
            if (ret != 0) {
                k_msleep(5000);
                continue;
            }

            mqtt_client_init(&client_ctx);

            /* Setup broker address */
            client_ctx.broker = &broker_addr;
            client_ctx.evt_cb = mqtt_evt_handler;
            client_ctx.client_id = client_id;
            client_ctx.password = NULL;
            client_ctx.user_name = NULL;
            
            client_ctx.rx_buf = rx_buffer;
            client_ctx.rx_buf_size = sizeof(rx_buffer);
            client_ctx.tx_buf = tx_buffer;
            client_ctx.tx_buf_size = sizeof(tx_buffer);

            /* We use plain TCP */
            client_ctx.transport.type = MQTT_TRANSPORT_NON_SECURE;

            ret = mqtt_connect(&client_ctx);
            if (ret != 0) {
                LOG_ERR("mqtt_connect failed: %d", ret);
                k_msleep(5000);
                continue;
            }
        }

        ret = mqtt_input(&client_ctx);
        if (ret < 0) {
            LOG_ERR("mqtt_input error: %d", ret);
            mqtt_abort(&client_ctx);
            connected = false;
        }

        ret = mqtt_live(&client_ctx);
        if (ret != 0 && ret != -EAGAIN) {
            LOG_ERR("mqtt_live error: %d", ret);
            mqtt_abort(&client_ctx);
            connected = false;
        }

        k_msleep(100);
    }
}

void mqtt_ctrl_start(void)
{
    LOG_INF("Starting MQTT thread...");
    k_thread_create(&mqtt_thread_data,
                    mqtt_thread_stack,
                    K_THREAD_STACK_SIZEOF(mqtt_thread_stack),
                    mqtt_thread_fn,
                    NULL, NULL, NULL,
                    MQTT_THREAD_PRIORITY,
                    0,
                    K_NO_WAIT);
}
