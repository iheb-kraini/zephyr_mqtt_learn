#include <stdbool.h>
#include <string.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_mqtt, LOG_LEVEL_DBG);

#include "mqtt_client.h"
#include <zephyr/data/json.h>
#include <zephyr/kernel.h>
#include <zephyr/net/mqtt.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/posix/netdb.h>
#include <zephyr/posix/poll.h>
#include <zephyr/posix/sys/socket.h>
#include <zephyr/random/random.h>

extern bool led_state;

struct led_cmd {
  const char *led;
};

static const struct json_obj_descr led_cmd_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct led_cmd, led, JSON_TOK_STRING),
};

/* Buffers for MQTT client */
static uint8_t rx_buffer[256];
static uint8_t tx_buffer[256];

// /* MQTT payload buffer */
// static uint8_t payload_buf[256];

/* MQTT broker details */
static struct sockaddr_storage broker;

/* Socket descriptor */
static struct pollfd fds[1];
static int nfds;

/* MQTT connectivity status flag */
bool mqtt_connected;

#if defined(CONFIG_MQTT_LIB_TLS)
#include "cert.h"

#define TLS_SNI_HOSTNAME CONFIG_NET_SAMPLE_MQTT_BROKER_HOSTNAME
#define APP_CA_CERT_TAG 1

static const sec_tag_t m_sec_tags[] = {
    APP_CA_CERT_TAG,
};

static int tls_init(void) {
  int rc;

  rc = tls_credential_add(APP_CA_CERT_TAG, TLS_CREDENTIAL_CA_CERTIFICATE,
                          ca_certificate, sizeof(ca_certificate));
  if (rc < 0) {
    LOG_ERR("Failed to register public certificate: %d", rc);
    return rc;
  }

  return rc;
}
#endif

// /* MQTT client ID buffer */
// static uint8_t client_id[50];

static void prepare_fds(struct mqtt_client *client) {
  if (client->transport.type == MQTT_TRANSPORT_NON_SECURE) {
    fds[0].fd = client->transport.tcp.sock;
  }
#if defined(CONFIG_MQTT_LIB_TLS)
  else if (client->transport.type == MQTT_TRANSPORT_SECURE) {
    fds[0].fd = client->transport.tls.sock;
  }
#endif

  fds[0].events = POLLIN;
  nfds = 1;
}

static void clear_fds(void) { nfds = 0; }

static inline void on_mqtt_connect(void) {
  mqtt_connected = true;
  LOG_INF("Connected to MQTT broker!");
  LOG_INF("Hostname: %s", CONFIG_NET_SAMPLE_MQTT_BROKER_HOSTNAME);
  LOG_INF("Port: %s", CONFIG_NET_SAMPLE_MQTT_BROKER_PORT);
}

static inline void on_mqtt_disconnect(void) {
  mqtt_connected = false;
  clear_fds();
  LOG_INF("Disconnected from MQTT broker");
}

/** Called when an MQTT payload is received.
 *  Reads the payload and calls the commands
 *  handler if a payloads is received on the
 *  command topic
 */
static void on_mqtt_publish(struct mqtt_client *const client,
                            const struct mqtt_evt *evt) {
  int rc;
  uint8_t payload[256];

  rc = mqtt_read_publish_payload(client, payload, sizeof(payload) - 1);
  if (rc < 0) {
    LOG_ERR("Failed to read received MQTT payload [%d]", rc);
    return;
  }
  /* Place null terminator at end of payload buffer */
  payload[rc] = '\0';

  LOG_INF("MQTT payload received!");
  LOG_INF("topic: '%s', payload: %s",
          evt->param.publish.message.topic.topic.utf8, payload);

  if (strcmp(evt->param.publish.message.topic.topic.utf8, CONFIG_READ_TOPIC) ==
      0) {
    struct led_cmd data;
    int ret;

    ret = json_obj_parse((char *)payload, 256, led_cmd_descr,
                         ARRAY_SIZE(led_cmd_descr), &data);
    if (ret < 0) {
      LOG_ERR("Invalid JSON payload received");
      return;
    }
    LOG_INF("payload is :\n%s", payload);

    if (data.led != NULL) {
      if (strcmp(data.led, "on") == 0) {
        led_state = true;
      } else if (strcmp(data.led, "off") == 0) {
        led_state = false;
      }
    }
  }
}

/** Handler for asynchronous MQTT events */
static void mqtt_event_handler(struct mqtt_client *const client,
                               const struct mqtt_evt *evt) {
  switch (evt->type) {
  case MQTT_EVT_CONNACK:
    if (evt->result != 0) {
      LOG_ERR("MQTT Event Connect failed [%d]", evt->result);
      break;
    }
    on_mqtt_connect();
    break;

  case MQTT_EVT_DISCONNECT:
    on_mqtt_disconnect();
    break;

  case MQTT_EVT_PINGRESP:
    LOG_INF("PINGRESP packet");
    break;

  case MQTT_EVT_PUBACK:
    if (evt->result != 0) {
      LOG_ERR("MQTT PUBACK error [%d]", evt->result);
      break;
    }

    LOG_INF("PUBACK packet ID: %u", evt->param.puback.message_id);
    break;

  case MQTT_EVT_PUBREC:
    if (evt->result != 0) {
      LOG_ERR("MQTT PUBREC error [%d]", evt->result);
      break;
    }

    LOG_INF("PUBREC packet ID: %u", evt->param.pubrec.message_id);

    const struct mqtt_pubrel_param rel_param = {
        .message_id = evt->param.pubrec.message_id};

    mqtt_publish_qos2_release(client, &rel_param);
    break;

  case MQTT_EVT_PUBREL:
    if (evt->result != 0) {
      LOG_ERR("MQTT PUBREL error [%d]", evt->result);
      break;
    }

    LOG_INF("PUBREL packet ID: %u", evt->param.pubrel.message_id);

    const struct mqtt_pubcomp_param rec_param = {
        .message_id = evt->param.pubrel.message_id};

    mqtt_publish_qos2_complete(client, &rec_param);
    break;

  case MQTT_EVT_PUBCOMP:
    if (evt->result != 0) {
      LOG_ERR("MQTT PUBCOMP error %d", evt->result);
      break;
    }

    LOG_INF("PUBCOMP packet ID: %u", evt->param.pubcomp.message_id);
    break;

  case MQTT_EVT_SUBACK:
    if (evt->result == MQTT_SUBACK_FAILURE) {
      LOG_ERR("MQTT SUBACK error [%d]", evt->result);
      break;
    }

    LOG_INF("SUBACK packet ID: %d", evt->param.suback.message_id);
    break;

  case MQTT_EVT_PUBLISH:
    const struct mqtt_publish_param *p = &evt->param.publish;

    if (p->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
      const struct mqtt_puback_param ack_param = {.message_id = p->message_id};
      mqtt_publish_qos1_ack(client, &ack_param);
    } else if (p->message.topic.qos == MQTT_QOS_2_EXACTLY_ONCE) {
      const struct mqtt_pubrec_param rec_param = {.message_id = p->message_id};
      mqtt_publish_qos2_receive(client, &rec_param);
    }

    on_mqtt_publish(client, evt);

  default:
    break;
  }
}

/** Poll the MQTT socket for received data */
static int poll_mqtt_socket(struct mqtt_client *client, int timeout) {
  int rc;

  prepare_fds(client);

  if (nfds <= 0) {
    return -EINVAL;
  }

  rc = poll(fds, nfds, timeout);
  if (rc < 0) {
    LOG_ERR("Socket poll error [%d]", rc);
  }

  return rc;
}

// /** Retrieves a sensor sample and encodes it in JSON format */
// static int get_mqtt_payload(struct mqtt_binstr *payload) {
//   int rc;
//   struct sensor_sample sample;

//   rc = device_read_sensor(&sample);
//   if (rc != 0) {
//     LOG_ERR("Failed to get sensor sample [%d]", rc);
//     return rc;
//   }

//   rc = json_obj_encode_buf(sensor_sample_descr,
//   ARRAY_SIZE(sensor_sample_descr),
//                            &sample, payload_buf, 256);
//   if (rc != 0) {
//     LOG_ERR("Failed to encode JSON object [%d]", rc);
//     return rc;
//   }

//   payload->data = payload_buf;
//   payload->len = strlen(payload->data);

//   return rc;
// }

int app_mqtt_publish(struct mqtt_client *client, const char *topic,
                     const char *data) {
  int rc;
  struct mqtt_publish_param param;
  static uint16_t msg_id = 1;
  struct mqtt_topic mq_topic = {.topic = {.utf8 = topic, .size = strlen(topic)},
                                .qos = MQTT_QOS_1_AT_LEAST_ONCE};

  param.message.topic = mq_topic;
  param.message.payload.data = (uint8_t *)data;
  param.message.payload.len = strlen(data);
  param.message_id = msg_id++;
  param.dup_flag = 0;
  param.retain_flag = 0;

  rc = mqtt_publish(client, &param);
  if (rc != 0) {
    LOG_ERR("MQTT Publish failed [%d]", rc);
  }

  // LOG_INF("Published to topic '%s', QoS %d", param.message.topic.topic.utf8,
  //         param.message.topic.qos);

  return rc;
}

int app_mqtt_subscribe(struct mqtt_client *client, const char *topic) {
  int rc;
  struct mqtt_topic sub_topics[] = {
      {.topic = {.utf8 = topic, .size = strlen(topic)},
       .qos = MQTT_QOS_1_AT_LEAST_ONCE}};
  const struct mqtt_subscription_list sub_list = {.list = sub_topics,
                                                  .list_count =
                                                      ARRAY_SIZE(sub_topics),
                                                  .message_id = 5841u};

  LOG_INF("Subscribing to %d topic(s)", sub_list.list_count);

  rc = mqtt_subscribe(client, &sub_list);
  if (rc != 0) {
    LOG_ERR("MQTT Subscribe failed [%d]", rc);
  }

  return rc;
}

/** Process incoming MQTT data and keep the connection alive*/
int app_mqtt_process(struct mqtt_client *client) {
  int rc;

  rc = poll_mqtt_socket(client, mqtt_keepalive_time_left(client));
  if (rc != 0) {
    if (fds[0].revents & POLLIN) {
      /* MQTT data received */
      rc = mqtt_input(client);
      if (rc != 0) {
        LOG_ERR("MQTT Input failed [%d]", rc);
        return rc;
      }
      /* Socket error */
      if (fds[0].revents & (POLLHUP | POLLERR)) {
        LOG_ERR("MQTT socket closed / error");
        return -ENOTCONN;
      }
    }
  } else {
    /* Socket poll timed out, time to call mqtt_live() */
    rc = mqtt_live(client);
    if (rc != 0) {
      LOG_ERR("MQTT Live failed [%d]", rc);
      return rc;
    }
  }

  return 0;
}

void app_mqtt_run(struct mqtt_client *client) {
  int rc;

  /* Thread will primarily remain in this loop */
  while (mqtt_connected) {
    rc = app_mqtt_process(client);
    if (rc != 0) {
      break;
    }
  }
  /* Gracefully close connection */
  mqtt_disconnect(client, NULL);
}

void app_mqtt_connect(struct mqtt_client *client) {
  int rc = 0;

  mqtt_connected = false;

  /* Block until MQTT CONNACK event callback occurs */
  while (!mqtt_connected) {
    rc = mqtt_connect(client);
    if (rc != 0) {
      LOG_ERR("MQTT Connect failed [%d]", rc);
      k_msleep(MSECS_WAIT_RECONNECT);
      continue;
    }

    /* Poll MQTT socket for response */
    rc = poll_mqtt_socket(client, MSECS_NET_POLL_TIMEOUT);
    if (rc > 0) {
      mqtt_input(client);
    }

    if (!mqtt_connected) {
      mqtt_abort(client);
    }
  }
}

int app_mqtt_init(struct mqtt_client *client) {
  int rc;
  uint8_t broker_ip[NET_IPV4_ADDR_LEN];
  struct sockaddr_in *broker4;
  struct addrinfo *result;
  const struct addrinfo hints = {.ai_family = AF_INET,
                                 .ai_socktype = SOCK_STREAM};

  /* Resolve IP address of MQTT broker */
  rc = getaddrinfo(CONFIG_NET_SAMPLE_MQTT_BROKER_HOSTNAME,
                   CONFIG_NET_SAMPLE_MQTT_BROKER_PORT, &hints, &result);
  if (rc != 0) {
    LOG_ERR("Failed to resolve broker hostname [%s]", gai_strerror(rc));
    return -EIO;
  }
  if (result == NULL) {
    LOG_ERR("Broker address not found");
    return -ENOENT;
  }

  broker4 = (struct sockaddr_in *)&broker;
  broker4->sin_addr.s_addr =
      ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
  broker4->sin_family = AF_INET;
  broker4->sin_port = ((struct sockaddr_in *)result->ai_addr)->sin_port;
  freeaddrinfo(result);

  /* Log resolved IP address */
  inet_ntop(AF_INET, &broker4->sin_addr.s_addr, broker_ip, sizeof(broker_ip));
  LOG_INF("Connecting to MQTT broker @ %s", broker_ip);

  /* MQTT client configuration */
  // init_mqtt_client_id();
  mqtt_client_init(client);
#if defined(CONFIG_MQTT_LIB_TLS)
  rc = tls_init();
  if (rc != 0) {
    LOG_ERR("TLS init failed [%d]", rc);
    return rc;
  }

  struct mqtt_sec_config *tls_config = &client->transport.tls.config;

  tls_config->peer_verify = TLS_PEER_VERIFY_REQUIRED;
  tls_config->cipher_list = NULL;
  tls_config->cipher_count = 0;
  tls_config->sec_tag_list = m_sec_tags;
  tls_config->sec_tag_count = ARRAY_SIZE(m_sec_tags);
  tls_config->hostname = TLS_SNI_HOSTNAME;

  client->transport.type = MQTT_TRANSPORT_SECURE;
#endif
  client->broker = &broker;
  client->evt_cb = mqtt_event_handler;
  client->client_id.utf8 = (uint8_t *)"my_device";
  client->client_id.size = strlen("my_device");
  client->password = NULL;
  client->user_name = NULL;
  client->protocol_version = MQTT_VERSION_3_1_1;

  /* MQTT buffers configuration */
  client->rx_buf = rx_buffer;
  client->rx_buf_size = sizeof(rx_buffer);
  client->tx_buf = tx_buffer;
  client->tx_buf_size = sizeof(tx_buffer);

  return rc;
}
