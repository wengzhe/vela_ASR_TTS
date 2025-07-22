/****************************************************************************
 * frameworks/ai/src/volc/ai_volc_tts.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <json_object.h>
#include <json_tokener.h>
#include <libwebsockets.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <uuid.h>
#include <uv.h>
#include <uv_async_queue.h>

#include "ai_common.h"
#include "ai_tts_plugin.h"

// Message Type
#define VOLC_PROTOCOL_VERSION 0x01
#define VOLC_DEFAULT_HEADER_SIZE 0x01
#define VOLC_FULL_CLIENT_REQUEST 0x01
#define VOLC_FULL_SERVER_RESPONSE 0x09
#define VOLC_AUDIO_ONLY_RESPONSE 0x0B
#define VOLC_SERVER_ACK 0x0B
#define VOLC_SERVER_ERROR_RESPONSE 0x0F

// Message Type Specific Flags
#define VOLC_NO_SEQUENCE 0x00
#define VOLC_POS_SEQUENCE 0x01
#define VOLC_LAST_NO_SEQUENCE 0x02
#define VOLC_NEG_SEQUENCE 0x03
#define VOLC_FLAG_EVENT 0x04

// Message Serialization
#define VOLC_NO_SERIALIZATION 0x00
#define VOLC_JSON 0x01

// Message Compression
#define VOLC_NO_COMPRESSION 0x00
#define VOLC_GZIP 0x01

// Message Event
#define VOLC_EVENT_NONE 0
#define VOLC_EVENT_START_CONNECTION 1
#define VOLC_EVENT_FINISH_CONNECTION 2
#define VOLC_EVENT_CONNECTION_STARTED 50
#define VOLC_EVENT_CONNECTION_ERROR 51
#define VOLC_EVENT_CONNECTION_FINISHED 52
#define VOLC_EVENT_START_SESSION 100
#define VOLC_EVENT_FINISH_SESSION 102
#define VOLC_EVENT_SESSION_STARTED 150
#define VOLC_EVENT_SESSION_FINISHED 152
#define VOLC_EVENT_SESSION_FAILED 153
#define VOLC_EVENT_TASK_REQUEST 200
#define VOLC_EVENT_TTS_SENTENCE_START 350
#define VOLC_EVENT_TTS_SENTENCE_END 351
#define VOLC_EVENT_TTS_RESPONSE 352

#define VOLC_APP_ID "3306859263"
#define VOLC_ACCESS_TOKEN "LyWxL1O5wV4UMgqhSgjU6QnEcV_HJIaD"
#define VOLC_URL "wss://openspeech.bytedance.com/api/v3/tts/bidirection"
#define VOLC_HOST "openspeech.bytedance.com"
#define VOLC_PATH "/api/v3/tts/bidirection"
#define VOLC_CLIENT_PROTOCOL_NAME ""

#define VOLC_HEADER_LEN 12
#define VOLC_TIMEOUT 1000 // milliseconds

#define VOLC_LOOP_INTERVAL 10000

/****************************************************************************
 * Private Types
 ****************************************************************************/
struct volc_tts_context;

struct volc_tts_lws_state {
    struct volc_tts_context* ctx;
    struct lws_context* lws_ctx;
    struct lws* wsi;
    int conn_state;
    unsigned char* payload;
    char connect_id[37]; // 存储UUID字符串
    char session_id[37];
    unsigned char* recv_buf;
    unsigned char* recv_buf_ptr;
    int recv_buf_size;
};

typedef struct {
    int protocol_version;
    int header_size;
    int message_type;
    int message_type_specific_flags;
    int serialization_method;
    int message_compression;
    int reserved;
    int event;
    int payload_size;
    char* payload;
    int code;
    char* error_msg;
    char* data;
    int completed;
    int need_cb;
} volc_tts_response_result;

typedef struct volc_tts_context {
    tts_engine_callback_t cb;
    void* cookie;
    pthread_t thread;
    uv_loop_t loop;
    uv_async_queue_t* asyncq;
    tts_engine_uvasyncq_cb_t uvasyncq_cb;
    void* opaque;
    tts_engine_env_params_t* env_params;
    sem_t sem;
    char* cache_text;
    int cache_len;
    bool is_running;
    bool is_finished;
    bool is_closed;
    struct volc_tts_lws_state* state;
    tts_engine_audio_info_t audio_info;
} volc_tts_context_t;

typedef struct {
    int pb_code;
    tts_engine_error_t voice_code;
    char const* str_code;
} tts_engine_err_code_t;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void volc_tts_generate_uuid(char* str, int len)
{
    uuid_t uuid;
    char* uuid_str;
    uuid_create(&uuid, NULL);
    uuid_to_string(&uuid, &uuid_str, NULL);
    strlcpy(str, uuid_str, len);
    free(uuid_str);
}

static void volc_tts_int_to_bytes(int value, unsigned char* bytes)
{
    bytes[0] = (value >> 24) & 0xFF;
    bytes[1] = (value >> 16) & 0xFF;
    bytes[2] = (value >> 8) & 0xFF;
    bytes[3] = value & 0xFF;
}

int volc_tts_bytes_to_int(const unsigned char* bytes)
{
    return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

static void volc_tts_generate_message_header(unsigned char* header,
    uint8_t message_type,
    uint8_t sequence_flag,
    uint8_t serialization,
    uint8_t compression)
{
    header[0] = VOLC_PROTOCOL_VERSION << 4 | VOLC_DEFAULT_HEADER_SIZE;
    header[1] = message_type << 4 | sequence_flag;
    header[2] = serialization << 4 | compression;
    header[3] = 0;
}

static int volc_tts_parse_response(struct volc_tts_lws_state* state,
    const unsigned char* res,
    size_t length,
    volc_tts_response_result* result)
{
    unsigned char temp[4];
    size_t payload_len;
    int sid_len;

    if (res == NULL || length == 0)
        return -1;

    memset(result, 0, sizeof(volc_tts_response_result));

    const unsigned char num = 0b00001111;
    result->protocol_version = (res[0] >> 4) & num;
    result->header_size = res[0] & 0x0f;
    result->message_type = (res[1] >> 4) & num;
    result->message_type_specific_flags = res[1] & 0x0f;
    result->serialization_method = res[2] >> num;
    result->message_compression = res[2] & 0x0f;
    result->reserved = res[3];

    if (result->message_type == VOLC_FULL_SERVER_RESPONSE || result->message_type == VOLC_AUDIO_ONLY_RESPONSE) {
        if (result->message_type_specific_flags == VOLC_FLAG_EVENT) {
            memcpy(temp, res + 4, sizeof(temp));
            result->event = volc_tts_bytes_to_int(temp);
        }

        switch (result->event) {
        case VOLC_EVENT_CONNECTION_STARTED:
            AI_INFO("tts_volc connect started\n");
            state->conn_state = VOLC_EVENT_CONNECTION_STARTED;
            break;
        case VOLC_EVENT_CONNECTION_ERROR:
            AI_INFO("tts_volc connect failed\n");
            break;
        case VOLC_EVENT_SESSION_STARTED:
            AI_INFO("tts_volc session started\n");
            state->conn_state = VOLC_EVENT_SESSION_STARTED;
            break;
        case VOLC_EVENT_TTS_RESPONSE:
            if (result->message_type == VOLC_AUDIO_ONLY_RESPONSE) {
                memcpy(temp, res + 8, sizeof(temp));
                sid_len = volc_tts_bytes_to_int(temp);
                memcpy(temp, res + 12 + sid_len, sizeof(temp));
                result->payload_size = volc_tts_bytes_to_int(temp);

                if (result->payload_size > 0) {
                    AI_INFO("tts_volc audio only response len:%d\n", result->payload_size);
                    result->data = (char*)malloc(result->payload_size);
                    memcpy(result->data, res + 16 + sid_len, result->payload_size);
                    result->need_cb = 1;
                } else
                    AI_INFO("tts_volc audio only response null!\n");
            } else
                AI_INFO("tts_volc full response\n");
            break;
        case VOLC_EVENT_SESSION_FAILED:
        case VOLC_EVENT_SESSION_FINISHED:
            AI_INFO("tts_volc session failed or finished\n");
            break;
        case VOLC_EVENT_TTS_SENTENCE_START:
            AI_INFO("tts_volc sentence start!\n");
            break;
        case VOLC_EVENT_TTS_SENTENCE_END:
            result->payload_size = 0;
            result->data = NULL;
            result->need_cb = 1;
            result->completed = 1;
            AI_INFO("tts_volc sentence end!\n");
            break;
        default:
            AI_INFO("tts_volc other event:%d\n", result->event);
        }
    } else if (result->message_type == VOLC_SERVER_ERROR_RESPONSE) {
        memcpy(temp, res + 4, sizeof(temp));
        result->event = volc_tts_bytes_to_int(temp);

        memcpy(temp, res + 8, sizeof(temp));
        result->payload_size = volc_tts_bytes_to_int(temp);

        payload_len = length - VOLC_HEADER_LEN;
        result->payload = malloc(payload_len + 1);
        if (result->payload == NULL) {
            return -1;
        }
        memcpy(result->payload, res + VOLC_HEADER_LEN, payload_len);
        result->payload[payload_len] = '\0';

        result->code = result->event;
        result->error_msg = result->payload;
        result->need_cb = 1;
        AI_INFO("tts_volc response:{\"code\":%d,\"error msg\":%s}\n",
            result->code, result->error_msg);
    }

    if (result->payload) {
        free(result->payload);
        result->payload = NULL;
    }
    return result->event;
}

static void volc_tts_send_start_connection(struct volc_tts_lws_state* state)
{
    size_t message_size;
    unsigned char* message;
    int payload_len = 4;
    int dest_pos = LWS_PRE;
    unsigned char headers[4];
    unsigned char event[4];
    char* payload;
    int len;

    volc_tts_generate_message_header(headers, VOLC_FULL_CLIENT_REQUEST, VOLC_FLAG_EVENT, VOLC_JSON, VOLC_NO_COMPRESSION);

    volc_tts_int_to_bytes(VOLC_EVENT_START_CONNECTION, event);
    payload = "{}";

    message_size = sizeof(headers) + sizeof(event) + payload_len + strlen(payload);
    message = (unsigned char*)malloc(message_size + LWS_PRE);

    memcpy(message + dest_pos, headers, sizeof(headers));
    dest_pos += sizeof(headers);

    memcpy(message + dest_pos, event, sizeof(event));
    dest_pos += sizeof(event);

    volc_tts_int_to_bytes(strlen(payload), message + dest_pos);
    dest_pos += payload_len;

    memcpy(message + dest_pos, payload, strlen(payload));

    len = lws_write(state->wsi, message + LWS_PRE, message_size, LWS_WRITE_BINARY);
    if (len < message_size)
        AI_INFO("volc_tts_send_start_connection: len < message_size");

    AI_INFO("tts_volc send start connection\n");

    free(message);
    lws_callback_on_writable(state->wsi);
}

static void volc_tts_send_start_session(struct volc_tts_lws_state* state)
{
    const char* compressed;
    size_t compressed_len;
    size_t message_size;
    unsigned char* message;
    int payload_len = 4;
    int sid_len = 4;
    int dest_pos = LWS_PRE;
    unsigned char headers[4];
    unsigned char event[4];
    int len;

    struct json_object* payload = json_object_new_object();
    struct json_object* user = json_object_new_object();
    json_object_object_add(user, "uid", json_object_new_string("test"));
    json_object_object_add(payload, "user", user);
    json_object_object_add(payload, "event", json_object_new_int(VOLC_EVENT_START_SESSION));
    json_object_object_add(payload, "namespace", json_object_new_string("BidirectionalTTS"));

    struct json_object* request_params = json_object_new_object();
    json_object_object_add(request_params, "speaker", json_object_new_string("zh_female_shuangkuaisisi_moon_bigtts")); // zh_female_shuangkuaisisi_moon_bigtts

    struct json_object* audio = json_object_new_object();
    json_object_object_add(audio, "format", json_object_new_string("pcm"));
    json_object_object_add(audio, "sample_rate", json_object_new_int(state->ctx->audio_info.sample_rate));
    json_object_object_add(audio, "enable_timestamp", json_object_new_boolean(true));
    json_object_object_add(request_params, "audio_params", audio);

    json_object_object_add(payload, "req_params", request_params);
    const char* json_str = json_object_to_json_string(payload);

    compressed_len = strlen(json_str);
    compressed = json_str;

    volc_tts_generate_message_header(headers, VOLC_FULL_CLIENT_REQUEST, VOLC_FLAG_EVENT, VOLC_JSON, VOLC_NO_COMPRESSION);
    volc_tts_int_to_bytes(VOLC_EVENT_START_SESSION, event);

    message_size = sizeof(headers) + sizeof(event) + sid_len + strlen(state->session_id) + payload_len + compressed_len;
    message = malloc(message_size + LWS_PRE);

    memcpy(message + dest_pos, headers, sizeof(headers));
    dest_pos += sizeof(headers);

    memcpy(message + dest_pos, event, sizeof(event));
    dest_pos += sizeof(event);

    volc_tts_int_to_bytes(strlen(state->session_id), message + dest_pos);
    dest_pos += sid_len;

    memcpy(message + dest_pos, state->session_id, strlen(state->session_id));
    dest_pos += strlen(state->session_id);

    volc_tts_int_to_bytes(compressed_len, message + dest_pos);
    dest_pos += payload_len;

    memcpy(message + dest_pos, compressed, compressed_len);

    len = lws_write(state->wsi, message + LWS_PRE, message_size, LWS_WRITE_BINARY);
    if (len < message_size)
        AI_INFO("volc_tts_send_start_session: len < message_size");

    AI_INFO("tts_volc send start session:%s\n", json_str);

    free(message);
    // free(compressed);
    json_object_put(payload);
    lws_callback_on_writable(state->wsi);
}

static void volc_tts_send_text(struct volc_tts_lws_state* state)
{
    const char* compressed;
    size_t compressed_len;
    size_t message_size;
    unsigned char* message;
    int payload_len = 4;
    int sid_len = 4;
    int dest_pos = LWS_PRE;
    unsigned char headers[4];
    unsigned char event[4];
    int len;

    if (state->ctx->is_finished || !state->ctx->cache_text || strlen(state->ctx->cache_text) <= 0)
        return;

    struct json_object* payload = json_object_new_object();
    struct json_object* user = json_object_new_object();
    json_object_object_add(user, "uid", json_object_new_string("test"));
    json_object_object_add(payload, "user", user);
    json_object_object_add(payload, "event", json_object_new_int(VOLC_EVENT_TASK_REQUEST));
    json_object_object_add(payload, "namespace", json_object_new_string("BidirectionalTTS"));

    struct json_object* request_params = json_object_new_object();
    json_object_object_add(request_params, "text", json_object_new_string(state->ctx->cache_text));
    json_object_object_add(request_params, "speaker", json_object_new_string("zh_female_shuangkuaisisi_moon_bigtts"));

    struct json_object* audio = json_object_new_object();
    json_object_object_add(audio, "format", json_object_new_string("pcm"));
    json_object_object_add(audio, "sample_rate", json_object_new_int(state->ctx->audio_info.sample_rate));
    json_object_object_add(request_params, "audio_params", audio);

    json_object_object_add(payload, "req_params", request_params);
    const char* json_str = json_object_to_json_string(payload);

    compressed_len = strlen(json_str);
    compressed = json_str;

    volc_tts_generate_message_header(headers, VOLC_FULL_CLIENT_REQUEST, VOLC_FLAG_EVENT, VOLC_JSON, VOLC_NO_COMPRESSION);
    volc_tts_int_to_bytes(VOLC_EVENT_TASK_REQUEST, event);

    message_size = sizeof(headers) + sizeof(event) + sid_len + strlen(state->session_id) + payload_len + compressed_len;
    message = malloc(message_size + LWS_PRE);

    memcpy(message + dest_pos, headers, sizeof(headers));
    dest_pos += sizeof(headers);

    memcpy(message + dest_pos, event, sizeof(event));
    dest_pos += sizeof(event);

    volc_tts_int_to_bytes(strlen(state->session_id), message + dest_pos);
    dest_pos += sid_len;

    memcpy(message + dest_pos, state->session_id, strlen(state->session_id));
    dest_pos += strlen(state->session_id);

    volc_tts_int_to_bytes(compressed_len, message + dest_pos);
    dest_pos += payload_len;

    memcpy(message + dest_pos, compressed, compressed_len);

    len = lws_write(state->wsi, message + LWS_PRE, message_size, LWS_WRITE_BINARY);
    if (len < message_size)
        AI_INFO("volc_tts_send_text: len < message_size");

    free(message);
    // free(compressed);
    json_object_put(payload);
    lws_callback_on_writable(state->wsi);

    memset(state->ctx->cache_text, 0, state->ctx->cache_len);
}

static int volc_tts_callback_bigtts(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len);

static struct lws_protocols tts_protocols[] = {
    { VOLC_CLIENT_PROTOCOL_NAME, volc_tts_callback_bigtts, 0, 0 },
    { NULL, NULL, 0, 0 }
};

static int volc_tts_callback_bigtts(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len)
{
    struct volc_tts_lws_state* state = (struct volc_tts_lws_state*)user;
    int remaining = 0;
    int code = 0;
    int ret;

    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
        AI_INFO("tts_volc Add header\n");
        unsigned char** headers = (unsigned char**)in;
        unsigned char* end = (*headers) + len;
        volc_tts_generate_uuid(state->connect_id, sizeof(state->connect_id));

        ret = lws_add_http_header_by_name(wsi,
            (unsigned char*)"X-Api-App-Key:",
            (unsigned char*)VOLC_APP_ID,
            strlen(VOLC_APP_ID),
            headers, end);
        if (ret < 0)
            AI_INFO("Add X-Api-App-Key failed\n");

        ret = lws_add_http_header_by_name(wsi,
            (unsigned char*)"X-Api-Access-Key:",
            (unsigned char*)VOLC_ACCESS_TOKEN,
            strlen(VOLC_ACCESS_TOKEN),
            headers, end);
        if (ret < 0)
            AI_INFO("Add X-Api-Access-Key failed\n");

        ret = lws_add_http_header_by_name(wsi,
            (unsigned char*)"X-Api-Resource-Id:",
            (unsigned char*)"volc.service_type.10029",
            strlen("volc.service_type.10029"),
            headers, end);
        if (ret < 0)
            AI_INFO("Add X-Api-Resource-Id failed\n");

        ret = lws_add_http_header_by_name(wsi,
            (unsigned char*)"X-Api-Connect-Id:",
            (unsigned char*)state->connect_id,
            strlen(state->connect_id),
            headers, end);
        if (ret < 0)
            AI_INFO("Add X-Api-Connect-Id failed\n");

        ret = lws_add_http_header_by_name(wsi,
            (unsigned char*)"User-Agent:",
            (unsigned char*)"curl/7.81.0",
            strlen("curl/7.81.0"),
            headers, end);
        if (ret < 0)
            AI_INFO("Add User-Agent failed\n");

        ret = lws_add_http_header_by_name(wsi,
            (unsigned char*)"Accept:",
            (unsigned char*)"*/*",
            strlen("*/*"),
            headers, end);
        if (ret < 0)
            AI_INFO("Add Accept failed\n");
        break;
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
        AI_INFO("tts_volc Connected to server\n");
        lws_callback_on_writable(state->wsi);
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        // AI_INFO("tts_volc Received message of length %zu %d\n", len, lws_is_final_fragment(wsi));
        if (len <= 0)
            break;

        if (state->recv_buf_size <= 0)
            remaining = 0;
        else
            remaining = state->recv_buf_size - (state->recv_buf_ptr - state->recv_buf);

        if (!state->recv_buf || remaining < len) {
            state->recv_buf = (unsigned char*)realloc(state->recv_buf, state->recv_buf_size + len);
            if (!state->recv_buf) {
                AI_INFO("asr_volc realloc failed\n");
                break;
            }
            state->recv_buf_ptr = state->recv_buf + (state->recv_buf_size - remaining);
            state->recv_buf_size += len;
        }

        memcpy(state->recv_buf_ptr, in, len);
        state->recv_buf_ptr += len;
        if (!lws_is_final_fragment(state->wsi)) {
            lws_callback_on_writable(state->wsi);
            break;
        }

        volc_tts_response_result result;
        int frame_size = state->recv_buf_ptr - state->recv_buf;
        volc_tts_parse_response(state, state->recv_buf, frame_size, &result);
        state->recv_buf_ptr = state->recv_buf;

        if (state->ctx->cb && result.need_cb) {
            tts_engine_result_t cb_result;
            if (result.data) {
                cb_result.result = result.data;
                cb_result.len = result.payload_size;
                cb_result.error_code = 0;
                state->ctx->cb(tts_engine_event_result, &cb_result, state->ctx->cookie);
                free(result.data);
            } else if (result.completed) {
                cb_result.result = NULL;
                cb_result.len = result.payload_size;
                cb_result.error_code = 0;
                state->ctx->cb(tts_engine_event_result, &cb_result, state->ctx->cookie);
                AI_INFO("tts_volc result complete:%d\n", result.completed);
            } else if (result.error_msg && result.code != 0) {
                cb_result.error_code = tts_engine_error_unkonwn;
                cb_result.result = NULL;
                cb_result.len = 0;
                state->ctx->cb(tts_engine_event_error, &cb_result, state->ctx->cookie);
                AI_INFO("tts_volc result error:%d\n", result.code);
            } else {
                AI_INFO("tts_volc error result!");
            }
        }

        lws_callback_on_writable(state->wsi);
        break;
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        // AI_INFO("tts_volc Write message of length %zu\n", len);
        if (state->conn_state == VOLC_EVENT_NONE) {
            volc_tts_send_start_connection(state);
            state->conn_state = VOLC_EVENT_START_CONNECTION;
        } else if (state->conn_state == VOLC_EVENT_CONNECTION_STARTED) {
            volc_tts_send_start_session(state);
            state->conn_state = VOLC_EVENT_START_SESSION;
        } else if (state->conn_state == VOLC_EVENT_SESSION_STARTED)
            volc_tts_send_text(state);
        break;
    case LWS_CALLBACK_CLOSED:
        AI_INFO("tts_volc Connection closed\n");
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        AI_INFO("tts_volc Connection error: %s\n", in ? (char*)in : "(no error information)");
        if (state->ctx->cb) {
            tts_engine_result_t cb_result;
            cb_result.error_code = tts_engine_error_network;
            cb_result.result = NULL;
            cb_result.len = 0;
            state->ctx->cb(tts_engine_event_error, &cb_result, state->ctx->cookie);
            lws_cancel_service(state->lws_ctx);
        }
        break;
    case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
        AI_INFO("tts_volc established http\n");
        break;
    case LWS_CALLBACK_CLIENT_HTTP_DROP_PROTOCOL:
        AI_INFO("tts_volc drop protocol\n");
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        code = lws_http_client_http_response(wsi);
        AI_INFO("tts_volc Connection closed: code=%d, msg=%s\n", code, in ? (char*)in : "(no error information)");
        break;
    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
        AI_INFO("tts_volc Peer initiated close\n");
        if (len >= 2) {
            char* codeBuf = (char*)in;
            code = code | (0xff & codeBuf[0]) << 8;
            code = code | (0xff & codeBuf[1]);

            AI_INFO("tts_volc Peer close reason:%d\n", code);
        }
        break;
    default:
        AI_INFO("tts_volc Default reason %d \n", reason);
        break;
    }

    return 0;
}

static void volc_tts_remove_char(char* str, char c)
{
    if (str == NULL)
        return;

    char* dst = str;
    while (*str) {
        if (*str != c) {
            *dst++ = *str;
        }
        str++;
    }
    *dst = '\0';
}

static struct lws_context* volc_tts_create_websocket_connection(volc_tts_context_t* ctx)
{
    struct lws_context_creation_info info;
    struct lws_context* context;

    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = tts_protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context = lws_create_context(&info);
    if (!context) {
        AI_INFO("Failed to create context\n");
        return NULL;
    }

    // lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_DEBUG, NULL);

    ctx->state = (struct volc_tts_lws_state*)calloc(1, sizeof(struct volc_tts_lws_state));
    if (!ctx->state) {
        perror("calloc failed");
        lws_context_destroy(context);
        return NULL;
    }
    ctx->state->ctx = ctx;
    ctx->state->lws_ctx = context;
    ctx->state->conn_state = VOLC_EVENT_NONE;
    volc_tts_generate_uuid(ctx->state->session_id, sizeof(ctx->state->session_id));
    volc_tts_remove_char(ctx->state->session_id, '-');

    struct lws_client_connect_info ccinfo = { 0 };
    ccinfo.context = context;
    ccinfo.address = VOLC_HOST; // 121.228.130.195
    ccinfo.port = 443;
    ccinfo.path = VOLC_PATH;
    ccinfo.host = VOLC_HOST;
    ccinfo.origin = VOLC_HOST;
    ccinfo.protocol = tts_protocols[0].name;
    ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    ccinfo.userdata = ctx->state;

    ctx->state->wsi = lws_client_connect_via_info(&ccinfo);
    if (!ctx->state->wsi) {
        AI_INFO("Failed to create connection\n");
        lws_context_destroy(context);
        ctx->state->lws_ctx = NULL;
        free(ctx->state);
        ctx->state = NULL;
        return NULL;
    }

    AI_INFO("tts_volc Connected to server: %s\n", VOLC_URL);

    return context;
}

static void volc_tts_uv_handle_close(uv_handle_t* handle, void* arg)
{
    if (uv_is_active(handle) && !uv_is_closing(handle))
        uv_close(handle, NULL);
}

static int volc_tts_close_handle(void* engine, int sync)
{
    volc_tts_context_t* ctx = (volc_tts_context_t*)engine;

    if (engine == NULL)
        return -EINVAL;

    uv_walk(&ctx->loop, volc_tts_uv_handle_close, NULL);

    if (sync) {
        while (uv_loop_alive(&ctx->loop)) {
            uv_run(&ctx->loop, UV_RUN_ONCE);
        }
    }

    return 0;
}

static void volc_tts_init_data(volc_tts_context_t* ctx)
{
    ctx->is_running = false;
    ctx->is_finished = true;
    ctx->is_closed = false;
    ctx->audio_info.sample_rate = 16000;
    ctx->audio_info.channels = 1;
    ctx->audio_info.sample_bit = 16;
    strlcpy(ctx->audio_info.audio_type, "raw", sizeof(ctx->audio_info.audio_type));
}

static void volc_tts_destroy_lws_state(volc_tts_context_t* ctx)
{
    if (ctx->state) {
        if (ctx->cache_text) {
            free(ctx->cache_text);
            ctx->cache_text = NULL;
        }

        if (ctx->state->recv_buf) {
            free(ctx->state->recv_buf);
            ctx->state->recv_buf = NULL;
            ctx->state->recv_buf_ptr = NULL;
            ctx->state->recv_buf_size = 0;
        }

        free(ctx->state);
        ctx->state = NULL;
    }
}

static void volc_tts_destroy_data(volc_tts_context_t* ctx)
{
    sem_destroy(&ctx->sem);

    volc_tts_destroy_lws_state(ctx);

    if (ctx->env_params) {
        free(ctx->env_params);
        ctx->env_params = NULL;
    }

    free(ctx);
}

static void* volc_tts_uvloop_thread(void* arg)
{
    volc_tts_context_t* ctx = (volc_tts_context_t*)arg;
    int ret;

    volc_tts_init_data(ctx);

    ret = uv_loop_init(&ctx->loop);
    if (ret < 0) {
        return NULL;
    }

    if (ctx->uvasyncq_cb) {
        ctx->asyncq = (uv_async_queue_t*)malloc(sizeof(uv_async_queue_t));
        ctx->asyncq->data = ctx->opaque;
        ret = uv_async_queue_init(&ctx->loop, ctx->asyncq, ctx->uvasyncq_cb);
        if (ret < 0)
            goto out;
        AI_INFO("tts_asyncq_init:%p", ctx->asyncq);
    }

    AI_INFO("[%s][%d] tts_running:%d\n", __func__, __LINE__, uv_loop_alive(&ctx->loop));

    while (uv_loop_alive(&ctx->loop) && !ctx->is_closed) {
        ret = uv_run(&ctx->loop, UV_RUN_NOWAIT);
        if (ret == 0)
            break;

        if (!ctx->is_finished && ctx->state && ctx->state->lws_ctx) {
            ret = lws_service(ctx->state->lws_ctx, -1);
            if (ret < 0) {
                AI_INFO("tts_service failed\n");
                if (ctx->cb) {
                    tts_engine_result_t cb_result;
                    cb_result.error_code = tts_engine_error_network;
                    cb_result.result = NULL;
                    cb_result.len = 0;
                    ctx->cb(tts_engine_event_error, &cb_result, ctx->cookie);
                }
                break;
            }
        } else if (ctx->is_finished && ctx->state && ctx->state->lws_ctx) {
            lws_context_destroy(ctx->state->lws_ctx);
            ctx->state->lws_ctx = NULL;
            volc_tts_destroy_lws_state(ctx);
            AI_INFO("tts_service stopped!\n");
        }

        if (!ctx->is_running) {
            sem_post(&ctx->sem);
            ctx->is_running = true;
        }

        usleep(VOLC_LOOP_INTERVAL);
    }

    sem_post(&ctx->sem);

    if (ctx->state && ctx->state->lws_ctx) {
        lws_context_destroy(ctx->state->lws_ctx);
        ctx->state->lws_ctx = NULL;
    }

    volc_tts_close_handle(ctx, 1);
    uv_stop(&ctx->loop);

out:
    if (ctx->asyncq) {
        free(ctx->asyncq);
        ctx->asyncq = NULL;
    }
    ret = uv_loop_close(&ctx->loop);
    ctx->is_running = false;
    volc_tts_destroy_data(ctx);
    AI_INFO("[%s][%d] tts_thread_out:%d\n", __func__, __LINE__, ret);

    return NULL;
}

static int volc_tts_create_thread(volc_tts_context_t* ctx)
{
    struct sched_param param;
    pthread_attr_t attr;
    int ret;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384);
    param.sched_priority = 110;
    pthread_attr_setschedparam(&attr, &param);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&ctx->thread, &attr, volc_tts_uvloop_thread, ctx);
    if (ret != 0) {
        AI_INFO("pthread_create failed\n");
        return ret;
    }
    pthread_setname_np(ctx->thread, "ai_volc");
    pthread_attr_destroy(&attr);

    return ret;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

static int volc_tts_init(void* engine, const tts_engine_init_params_t* param)
{
    volc_tts_context_t* ctx = (volc_tts_context_t*)engine;
    int ret;

    if (engine == NULL || param == NULL) {
        free(ctx);
        return -EINVAL;
    }

    sem_init(&ctx->sem, 0, 0);

    ctx->uvasyncq_cb = param->cb;
    ctx->opaque = param->opaque;
    ctx->is_running = false;
    ret = volc_tts_create_thread(ctx);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += VOLC_TIMEOUT % 1000 * 1000000;
    ts.tv_sec += ts.tv_nsec / 1000000000 + VOLC_TIMEOUT / 1000;
    ts.tv_nsec %= 1000000000;

    if (sem_timedwait(&ctx->sem, &ts) == -1) {
        if (errno == ETIMEDOUT)
            AI_INFO("sem_timedwait: wait ret timeout\n");
        perror("sem_timedwait: wait error\n");
        ret = -ETIMEDOUT;
    }

    AI_INFO("tts_volc_init");
    return ret;
}

static int volc_tts_uninit(void* engine)
{
    volc_tts_context_t* ctx = (volc_tts_context_t*)engine;

    if (engine == NULL)
        return -EINVAL;

    ctx->cb = NULL;
    ctx->cookie = NULL;
    ctx->is_closed = true;

    return 0;
}

static int volc_tts_event_cb(void* engine, tts_engine_callback_t callback, void* cookie)
{
    volc_tts_context_t* ctx = (volc_tts_context_t*)engine;

    if (engine == NULL)
        return -EINVAL;

    ctx->cb = callback;
    ctx->cookie = cookie;
    return 0;
}

static int volc_tts_speak(void* engine, const char* text, const tts_engine_audio_info_t* audio_info)
{
    volc_tts_context_t* ctx = (volc_tts_context_t*)engine;
    struct lws_context* context;

    if (engine == NULL || !text || strlen(text) == 0)
        return -EINVAL;

    if (audio_info != NULL) {
        ctx->audio_info.version = audio_info->version;
        ctx->audio_info.sample_rate = audio_info->sample_rate;
        ctx->audio_info.channels = audio_info->channels;
        strlcpy(ctx->audio_info.audio_type, audio_info->audio_type, sizeof(ctx->audio_info.audio_type));
        ctx->audio_info.sample_bit = audio_info->sample_bit;
    }

    if (!ctx->is_running)
        return -EPERM;

    if (ctx->cache_text == NULL || ctx->cache_len < strlen(text) + 1) {
        ctx->cache_text = realloc(ctx->cache_text, strlen(text) + 1);
        if (!ctx->cache_text)
            return -ENOMEM;
        ctx->cache_len = strlen(text) + 1;
    }
    strlcpy(ctx->cache_text, text, ctx->cache_len);

    if (!ctx->state || !ctx->state->lws_ctx) {
        context = volc_tts_create_websocket_connection(ctx);
        if (context == NULL) {
            AI_INFO("tts_create_connect failed\n");
            return -ENOTCONN;
        }
    }

    ctx->is_finished = false;
    lws_callback_on_writable(ctx->state->wsi);

    return 0;
}

static int volc_tts_stop(void* engine)
{
    volc_tts_context_t* ctx = (volc_tts_context_t*)engine;

    if (engine == NULL)
        return -EINVAL;

    ctx->is_finished = true;
    ctx->state->conn_state = VOLC_EVENT_NONE;

    if (ctx->state == NULL) {
        AI_INFO("tts_volc_tts_stop: state is NULL\n");
        return -EINVAL;
    }

    if (ctx->cache_text) {
        free(ctx->cache_text);
        ctx->cache_text = NULL;
    }

    if (ctx->state->recv_buf)
        ctx->state->recv_buf_ptr = ctx->state->recv_buf;

    lws_callback_on_writable(ctx->state->wsi);

    return 0;
}

static tts_engine_env_params_t* volc_tts_get_env_params(void* engine)
{
    volc_tts_context_t* ctx = (volc_tts_context_t*)engine;
    tts_engine_env_params_t* env_params;

    if (engine == NULL)
        return NULL;

    if (ctx->env_params) {
        AI_INFO("volc_tts_get_env_params exist");
        return ctx->env_params;
    }

    env_params = (tts_engine_env_params_t*)malloc(sizeof(tts_engine_env_params_t));
    env_params->format = "format=s16le:sample_rate=16000:ch_layout=mono";
    env_params->force_format = 1;
    env_params->loop = &ctx->loop;
    env_params->asyncq = ctx->asyncq;
    ctx->env_params = env_params;

    AI_INFO("volc_tts_get_env_params");

    return env_params;
}

tts_engine_plugin_t volc_tts_engine_plugin = {
    .name = "volc_tts",
    .priv_size = sizeof(volc_tts_context_t),
    .init = volc_tts_init,
    .uninit = volc_tts_uninit,
    .event_cb = volc_tts_event_cb,
    .speak = volc_tts_speak,
    .stop = volc_tts_stop,
    .get_env = volc_tts_get_env_params,
};
