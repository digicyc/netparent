#include "handler.h"
#include "log.h"
#include "nft.h"

#include <cjson/cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Extract the action segment from "netparent/<id>/cmd/<action>".
 * Returns a pointer into `topic` or NULL.
 */
static const char *parse_action(const char *topic, const char *device_id)
{
    char prefix[128];
    int n = snprintf(prefix, sizeof(prefix), "netparent/%s/cmd/", device_id);
    if (n < 0 || (size_t)n >= sizeof(prefix))
        return NULL;
    if (strncmp(topic, prefix, (size_t)n) != 0)
        return NULL;
    const char *action = topic + n;
    if (!*action)
        return NULL;
    return action;
}

static void reply(struct np_handler *h, const char *req_id,
                  bool ok, const char *action,
                  const char *detail_key, const char *detail_val)
{
    if (!req_id || !*req_id)
        return;
    cJSON *o = cJSON_CreateObject();
    if (!o)
        return;
    cJSON_AddBoolToObject(o, "ok", ok);
    if (action)
        cJSON_AddStringToObject(o, "action", action);
    if (detail_key && detail_val)
        cJSON_AddStringToObject(o, detail_key, detail_val);
    char *s = cJSON_PrintUnformatted(o);
    if (s) {
        np_mqtt_publish_response(h->mqtt, req_id, s);
        free(s);
    }
    cJSON_Delete(o);
}

struct list_ctx {
    cJSON *arr;
};

static void list_cb(const char *mac, void *user)
{
    struct list_ctx *c = user;
    cJSON_AddItemToArray(c->arr, cJSON_CreateString(mac));
}

void np_handle_message(struct np_handler *h,
                       const char *topic,
                       const void *payload, int paylen)
{
    const char *action = parse_action(topic, h->cfg->device_id);
    if (!action) {
        LOG_WARN("ignoring message on unrelated topic: %s", topic);
        return;
    }

    /* Copy payload into a null-terminated buffer for cJSON. */
    char *body = malloc((size_t)paylen + 1);
    if (!body) {
        LOG_ERR("oom processing message");
        return;
    }
    if (paylen > 0)
        memcpy(body, payload, (size_t)paylen);
    body[paylen] = '\0';

    cJSON *json = (paylen > 0) ? cJSON_Parse(body) : cJSON_CreateObject();
    if (!json) {
        LOG_WARN("invalid JSON on %s", topic);
        free(body);
        return;
    }

    const cJSON *jmac    = cJSON_GetObjectItemCaseSensitive(json, "mac");
    const cJSON *jreq    = cJSON_GetObjectItemCaseSensitive(json, "req_id");
    const char *mac      = cJSON_IsString(jmac) ? jmac->valuestring : NULL;
    const char *req_id   = cJSON_IsString(jreq) ? jreq->valuestring : NULL;

    LOG_INFO("cmd=%s mac=%s req_id=%s", action,
             mac ? mac : "(none)", req_id ? req_id : "(none)");

    if (!strcmp(action, "block")) {
        if (!mac) {
            reply(h, req_id, false, action, "error", "mac required");
        } else if (np_nft_block(h->cfg->nft_table, h->cfg->nft_set, mac) != 0) {
            reply(h, req_id, false, action, "error", "nft block failed");
        } else {
            reply(h, req_id, true, action, "mac", mac);
            np_leases_republish(h->leases);
        }
    } else if (!strcmp(action, "unblock")) {
        if (!mac) {
            reply(h, req_id, false, action, "error", "mac required");
        } else if (np_nft_unblock(h->cfg->nft_table, h->cfg->nft_set, mac) != 0) {
            reply(h, req_id, false, action, "error", "nft unblock failed");
        } else {
            reply(h, req_id, true, action, "mac", mac);
            np_leases_republish(h->leases);
        }
    } else if (!strcmp(action, "status")) {
        if (!mac) {
            reply(h, req_id, false, action, "error", "mac required");
        } else {
            int s = np_nft_is_blocked(h->cfg->nft_table, h->cfg->nft_set, mac);
            if (s < 0) {
                reply(h, req_id, false, action, "error", "lookup failed");
            } else {
                reply(h, req_id, true, action,
                      "blocked", s ? "true" : "false");
            }
        }
    } else if (!strcmp(action, "list")) {
        cJSON *o = cJSON_CreateObject();
        cJSON *arr = cJSON_CreateArray();
        struct list_ctx ctx = { .arr = arr };
        np_nft_list(h->cfg->nft_table, h->cfg->nft_set, list_cb, &ctx);
        cJSON_AddBoolToObject(o, "ok", true);
        cJSON_AddStringToObject(o, "action", action);
        cJSON_AddItemToObject(o, "blocked", arr);
        if (req_id) {
            char *s = cJSON_PrintUnformatted(o);
            if (s) {
                np_mqtt_publish_response(h->mqtt, req_id, s);
                free(s);
            }
        }
        cJSON_Delete(o);
    } else if (!strcmp(action, "ping")) {
        reply(h, req_id, true, action, "pong", "1");
    } else {
        LOG_WARN("unknown action: %s", action);
        reply(h, req_id, false, action, "error", "unknown action");
    }

    cJSON_Delete(json);
    free(body);
}
