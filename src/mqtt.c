#include "mqtt.h"
#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <mosquitto.h>

#define TOPIC_PREFIX_FMT "netparent/%s"
#define CMD_FILTER_FMT   "netparent/%s/cmd/#"
#define STATUS_FMT       "netparent/%s/status"
#define RESPONSE_FMT     "netparent/%s/response/%s"

struct np_mqtt {
    struct mosquitto *mosq;
    const struct np_config *cfg;
    np_mqtt_msg_cb on_message;
    void *user;

    np_mqtt_tick_cb tick_cb;
    void *tick_user;

    char *status_topic;
    char *cmd_filter;

    volatile sig_atomic_t stop;
};

static char *fmtstr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return NULL;
    }
    char *out = malloc((size_t)n + 1);
    if (!out) {
        va_end(ap2);
        return NULL;
    }
    vsnprintf(out, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return out;
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    struct np_mqtt *m = obj;
    if (rc != 0) {
        LOG_ERR("MQTT connect failed: %s", mosquitto_connack_string(rc));
        return;
    }
    LOG_INFO("MQTT connected; subscribing to %s", m->cmd_filter);
    if (mosquitto_subscribe(mosq, NULL, m->cmd_filter, 1) != MOSQ_ERR_SUCCESS) {
        LOG_ERR("subscribe failed");
        return;
    }
    /* Publish "online" status (retained). */
    np_mqtt_publish_status(m, "{\"online\":true}");
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)obj;
    if (rc == 0)
        LOG_INFO("MQTT disconnected cleanly");
    else
        LOG_WARN("MQTT disconnected unexpectedly (rc=%d), will reconnect", rc);
}

static void on_message(struct mosquitto *mosq, void *obj,
                       const struct mosquitto_message *msg)
{
    (void)mosq;
    struct np_mqtt *m = obj;
    if (!msg || !msg->topic)
        return;
    LOG_DBG("MQTT msg: %s (%d bytes)", msg->topic, msg->payloadlen);
    if (m->on_message)
        m->on_message(msg->topic, msg->payload, msg->payloadlen, m->user);
}

static void on_log(struct mosquitto *mosq, void *obj, int level, const char *str)
{
    (void)mosq;
    (void)obj;
    enum np_log_level lvl = NP_LOG_DBG;
    if (level & (MOSQ_LOG_ERR))
        lvl = NP_LOG_ERR;
    else if (level & MOSQ_LOG_WARNING)
        lvl = NP_LOG_WARN;
    else if (level & MOSQ_LOG_NOTICE)
        lvl = NP_LOG_INFO;
    np_log(lvl, "mosq: %s", str);
}

struct np_mqtt *np_mqtt_create(const struct np_config *cfg,
                               np_mqtt_msg_cb on_message_cb,
                               void *user)
{
    mosquitto_lib_init();

    struct np_mqtt *m = calloc(1, sizeof(*m));
    if (!m)
        return NULL;

    m->cfg = cfg;
    m->on_message = on_message_cb;
    m->user = user;
    m->status_topic = fmtstr(STATUS_FMT, cfg->device_id);
    m->cmd_filter   = fmtstr(CMD_FILTER_FMT, cfg->device_id);
    if (!m->status_topic || !m->cmd_filter)
        goto fail;

    /* Client ID: netparent-<device_id>, truncated by mosq if too long. */
    char client_id[64];
    snprintf(client_id, sizeof(client_id), "netparent-%s", cfg->device_id);

    m->mosq = mosquitto_new(client_id, true /*clean_session*/, m);
    if (!m->mosq) {
        LOG_ERR("mosquitto_new failed");
        goto fail;
    }

    mosquitto_connect_callback_set(m->mosq, on_connect);
    mosquitto_disconnect_callback_set(m->mosq, on_disconnect);
    mosquitto_message_callback_set(m->mosq, on_message);
    mosquitto_log_callback_set(m->mosq, on_log);

    if (cfg->username) {
        if (mosquitto_username_pw_set(m->mosq, cfg->username, cfg->password)
            != MOSQ_ERR_SUCCESS) {
            LOG_ERR("mosquitto_username_pw_set failed");
            goto fail;
        }
    }

    if (cfg->tls_enabled) {
        int rc = mosquitto_tls_set(m->mosq,
                                   cfg->tls_ca_file,
                                   NULL, /* capath */
                                   cfg->tls_cert_file,
                                   cfg->tls_key_file,
                                   NULL /* pw callback */);
        if (rc != MOSQ_ERR_SUCCESS) {
            LOG_ERR("mosquitto_tls_set failed: %s", mosquitto_strerror(rc));
            goto fail;
        }
        if (cfg->tls_insecure) {
            mosquitto_tls_insecure_set(m->mosq, true);
            LOG_WARN("TLS hostname verification DISABLED (tls_insecure=1)");
        }
    }

    /* LWT: offline message published by broker if we die. */
    if (mosquitto_will_set(m->mosq, m->status_topic,
                           (int)strlen("{\"online\":false}"),
                           "{\"online\":false}",
                           1 /*qos*/, true /*retain*/)
        != MOSQ_ERR_SUCCESS) {
        LOG_WARN("LWT set failed (continuing without LWT)");
    }

    /* Reconnect backoff: 2s..120s exponential. */
    mosquitto_reconnect_delay_set(m->mosq, 2, 120, true);

    return m;

fail:
    np_mqtt_destroy(m);
    return NULL;
}

int np_mqtt_run(struct np_mqtt *m)
{
    if (!m || !m->mosq)
        return -1;

    LOG_INFO("connecting to %s:%d (tls=%d)",
             m->cfg->broker_host, m->cfg->broker_port,
             m->cfg->tls_enabled);

    int rc = mosquitto_connect_async(m->mosq, m->cfg->broker_host,
                                     m->cfg->broker_port,
                                     m->cfg->keepalive);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_ERR("mosquitto_connect_async failed: %s",
                mosquitto_strerror(rc));
        return -1;
    }

    /* Loop with periodic check of stop flag. mosquitto_loop_forever
     * doesn't return on a signal-set flag, so we drive the loop
     * ourselves. */
    while (!m->stop) {
        rc = mosquitto_loop(m->mosq, 500, 1);
        if (rc == MOSQ_ERR_NO_CONN || rc == MOSQ_ERR_CONN_LOST) {
            LOG_WARN("connection lost; reconnecting");
            mosquitto_reconnect_async(m->mosq);
        } else if (rc != MOSQ_ERR_SUCCESS) {
            LOG_DBG("mosquitto_loop rc=%d (%s)", rc, mosquitto_strerror(rc));
        }
        if (m->tick_cb)
            m->tick_cb(m->tick_user);
    }

    /* Try to publish a clean "offline" before going down. */
    np_mqtt_publish_status(m, "{\"online\":false}");
    mosquitto_disconnect(m->mosq);
    /* Drain pending messages briefly. */
    for (int i = 0; i < 10; i++)
        mosquitto_loop(m->mosq, 100, 1);

    return 0;
}

void np_mqtt_stop(struct np_mqtt *m)
{
    if (m)
        m->stop = 1;
}

void np_mqtt_set_tick(struct np_mqtt *m, np_mqtt_tick_cb cb, void *user)
{
    if (!m)
        return;
    m->tick_cb = cb;
    m->tick_user = user;
}

int np_mqtt_publish_status(struct np_mqtt *m, const char *json_payload)
{
    if (!m || !m->mosq || !m->status_topic)
        return -1;
    int rc = mosquitto_publish(m->mosq, NULL, m->status_topic,
                               (int)strlen(json_payload), json_payload,
                               1, true /*retain*/);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_DBG("publish status rc=%d (%s)", rc, mosquitto_strerror(rc));
        return -1;
    }
    return 0;
}

int np_mqtt_publish_response(struct np_mqtt *m,
                             const char *suffix,
                             const char *json_payload)
{
    if (!m || !m->mosq || !suffix)
        return -1;
    char *topic = fmtstr(RESPONSE_FMT, m->cfg->device_id, suffix);
    if (!topic)
        return -1;
    int rc = mosquitto_publish(m->mosq, NULL, topic,
                               (int)strlen(json_payload), json_payload,
                               1, false);
    free(topic);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_DBG("publish response rc=%d (%s)", rc, mosquitto_strerror(rc));
        return -1;
    }
    return 0;
}

int np_mqtt_publish_devices(struct np_mqtt *m, const char *json_payload)
{
    if (!m || !m->mosq)
        return -1;
    char topic[128];
    snprintf(topic, sizeof(topic), "netparent/%s/devices", m->cfg->device_id);
    int rc = mosquitto_publish(m->mosq, NULL, topic,
                               (int)strlen(json_payload), json_payload,
                               1, true /*retain*/);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_DBG("publish devices rc=%d (%s)", rc, mosquitto_strerror(rc));
        return -1;
    }
    return 0;
}

int np_mqtt_publish_event(struct np_mqtt *m,
                          const char *kind,
                          const char *json_payload)
{
    if (!m || !m->mosq || !kind)
        return -1;
    char topic[160];
    snprintf(topic, sizeof(topic), "netparent/%s/event/%s",
             m->cfg->device_id, kind);
    int rc = mosquitto_publish(m->mosq, NULL, topic,
                               (int)strlen(json_payload), json_payload,
                               1, false);
    if (rc != MOSQ_ERR_SUCCESS) {
        LOG_DBG("publish event rc=%d (%s)", rc, mosquitto_strerror(rc));
        return -1;
    }
    return 0;
}

void np_mqtt_destroy(struct np_mqtt *m)
{
    if (!m)
        return;
    if (m->mosq) {
        mosquitto_destroy(m->mosq);
        m->mosq = NULL;
    }
    free(m->status_topic);
    free(m->cmd_filter);
    free(m);
    mosquitto_lib_cleanup();
}
