/****************************************************************************
 * frameworks/ai/src/asr/volc/ai_volc.c
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
#include "ai_ring_buffer.h"
#include "ai_voice_plugin.h"

#define VOLC_PROTOCOL_VERSION 0x01
#define VOLC_DEFAULT_HEADER_SIZE 0x01
#define VOLC_FULL_CLIENT_REQUEST 0x01
#define VOLC_AUDIO_ONLY_REQUEST 0x02
#define VOLC_FULL_SERVER_RESPONSE 0x09
#define VOLC_SERVER_ACK 0x0B
#define VOLC_SERVER_ERROR_RESPONSE 0x0F

#define VOLC_NO_SEQUENCE 0x00
#define VOLC_POS_SEQUENCE 0x01
#define VOLC_NEG_SEQUENCE 0x02
#define VOLC_NEG_WITH_SEQUENCE 0x03

#define VOLC_NO_SERIALIZATION 0x00
#define VOLC_JSON 0x01

#define VOLC_NO_COMPRESSION 0x00
#define VOLC_GZIP 0x01

#define VOLC_APP_ID "3306859263"
#define VOLC_ACCESS_TOKEN "LyWxL1O5wV4UMgqhSgjU6QnEcV_HJIaD"
#define VOLC_URL "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel"
#define VOLC_HOST "openspeech.bytedance.com"
#define VOLC_PATH "/api/v3/sauc/bigmodel"
#define VOLC_CLIENT_PROTOCOL_NAME ""

#define VOLC_HEADER_LEN 12
#define VOLC_TIMEOUT 1000 // milliseconds
#define VOLC_SILENCE_TIMEOUT 200000 // microseconds
#define VOLC_BUFFER_MAX_SIZE 128 * 1024

#define VOLC_LOOP_INTERVAL 10000

/****************************************************************************
 * Private Types
 ****************************************************************************/
struct volc_context;

struct volc_lws_state {
    struct volc_context* ctx;
    struct lws_context* lws_ctx;
    struct lws* wsi;
    int seq;
    ai_ring_buffer_t buffer;
    unsigned char* payload;
    char connect_id[37]; // 存储UUID字符串
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
    int sequence;
    int payload_size;
    char* payload;
    int code;
    char* error_msg;
    char* text;
    int completed;
} volc_response_result;

typedef struct volc_context {
    voice_callback_t cb;
    void* cookie;
    pthread_t thread;
    uv_loop_t loop;
    uv_async_queue_t* asyncq;
    ai_uvasyncq_cb_t uvasyncq_cb;
    void* opaque;
    voice_env_params_t* env_params;
    sem_t sem;
    bool is_running;
    bool is_finished;
    bool is_closed;
    struct volc_lws_state* state;
    voice_audio_info_t audio_info;
    char* app_id;
    char* app_key;
} volc_context_t;

typedef struct {
    int pb_code;
    voice_error_t voice_code;
    char const* str_code;
} voice_err_code_t;

static voice_err_code_t errcode_map[] = {
    { 0, 0, NULL },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void volc_generate_uuid(char* str, int len)
{
    uuid_t uuid;
    char* uuid_str;
    uuid_create(&uuid, NULL);
    uuid_to_string(&uuid, &uuid_str, NULL);
    strlcpy(str, uuid_str, len);
    free(uuid_str);
}

static void volc_int_to_bytes(int value, unsigned char* bytes)
{
    bytes[0] = (value >> 24) & 0xFF;
    bytes[1] = (value >> 16) & 0xFF;
    bytes[2] = (value >> 8) & 0xFF;
    bytes[3] = value & 0xFF;
}

int volc_bytes_to_int(const unsigned char* bytes)
{
    return (bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3];
}

__attribute__((used)) static int volc_gzip_compress(const unsigned char* input, size_t input_len, unsigned char** output, size_t* output_len)
{
    struct archive* a;
    struct archive_entry* entry;
    size_t used;

    a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_raw(a);
    archive_write_open_memory(a, *output, *output_len, &used);

    entry = archive_entry_new();
    archive_entry_set_size(entry, input_len);
    archive_write_header(a, entry);
    archive_write_data(a, input, input_len);
    archive_write_finish_entry(a);

    archive_entry_free(entry);
    archive_write_close(a);
    archive_write_free(a);

    *output_len = used;
    return ARCHIVE_OK;
}

static int volc_gzip_decompress(const unsigned char* input, size_t input_len, unsigned char** output, size_t* output_len)
{
    struct archive* a;
    struct archive_entry* entry;
    int ret;

    a = archive_read_new();
    archive_read_support_format_raw(a);
    archive_read_support_filter_gzip(a);

    ret = archive_read_open_memory(a, input, input_len);
    if (ret != ARCHIVE_OK) {
        return ret;
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        *output_len = archive_entry_size(entry);
        *output = malloc(*output_len);
        ret = archive_read_data(a, *output, *output_len);
        if (ret != ARCHIVE_OK) {
            *output_len = 0;
            free(*output);
            return ret;
        }
    }

    archive_read_free(a);
    return ARCHIVE_OK;
}

static void volc_generate_message_header(char* header,
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

static int volc_parse_response(const unsigned char* res, size_t length, volc_response_result* result)
{
    struct json_object* result_obj;
    struct json_object* result_text;
    const char* result_text_str;
    size_t output_len = 0;
    size_t payload_len;
    char* payloadStr = NULL;

    if (res == NULL || length == 0)
        return -1;

    memset(result, 0, sizeof(volc_response_result));

    const unsigned char num = 0b00001111;
    result->protocol_version = (res[0] >> 4) & num;
    result->header_size = res[0] & 0x0f;

    result->message_type = (res[1] >> 4) & num;
    result->message_type_specific_flags = res[1] & 0x0f;
    result->serialization_method = res[2] >> num;
    result->message_compression = res[2] & 0x0f;
    result->reserved = res[3];

    unsigned char temp[4];
    memcpy(temp, res + 4, sizeof(temp));
    result->sequence = volc_bytes_to_int(temp);

    memcpy(temp, res + 8, sizeof(temp));
    result->payload_size = volc_bytes_to_int(temp);

    payload_len = length - VOLC_HEADER_LEN;
    result->payload = malloc(payload_len + 1);
    if (result->payload == NULL) {
        return -1;
    }
    memcpy(result->payload, res + VOLC_HEADER_LEN, payload_len);
    result->payload[payload_len] = '\0';

    if (result->message_type_specific_flags == VOLC_NEG_SEQUENCE
        || result->message_type_specific_flags == VOLC_NEG_WITH_SEQUENCE) {
        result->completed = 1;
    }

    switch (result->message_type) {
    case VOLC_FULL_SERVER_RESPONSE:
        if (result->message_compression == VOLC_GZIP) {
            volc_gzip_decompress(res + VOLC_HEADER_LEN, payload_len, (unsigned char**)&payloadStr, &output_len);
            payload_len = output_len;
            free(payloadStr);
        } else {
            payloadStr = result->payload;
        }

        struct json_object* parsed_json = json_tokener_parse(payloadStr);
        json_object_object_get_ex(parsed_json, "result", &result_obj);
        json_object_object_get_ex(result_obj, "text", &result_text);
        result_text_str = json_object_get_string(result_text);
        if (result_text_str == NULL) {
            json_object_put(parsed_json);
            break;
        }
        result->text = (char*)malloc(strlen(result_text_str) + 1);
        if (result->text != NULL)
            strlcpy(result->text, result_text_str, strlen(result_text_str) + 1);
        json_object_put(parsed_json);
        break;

    case VOLC_SERVER_ACK:
        payloadStr = result->payload;
        AI_INFO("asr_volc payload:%s\n", payloadStr);
        break;

    case VOLC_SERVER_ERROR_RESPONSE:
        result->code = result->sequence;
        result->error_msg = result->payload;
        AI_INFO("asr_volc response:{\"code\":%d,\"error msg\":%s}\n",
            result->code, result->error_msg);
        break;

    default:
        AI_INFO("asr_volc response:{\"payload_size\":%d}\n", result->payload_size);
        break;
    }

    free(result->payload);
    return result->sequence;
}

__attribute__((used)) static void volc_free_response_result(volc_response_result* result)
{
    if (result->payload != NULL) {
        free(result->payload);
        result->payload = NULL;
    }
}

static void volc_send_initial_request(struct volc_lws_state* state)
{
    const char* compressed;
    size_t compressed_len;
    int payload_len = 4;
    int seq_len = 4;
    int dest_pos = LWS_PRE;
    char headers[4];
    int len;

    struct json_object* payload = json_object_new_object();
    struct json_object* user = json_object_new_object();
    json_object_object_add(user, "uid", json_object_new_string("test"));
    json_object_object_add(payload, "user", user);

    struct json_object* audio = json_object_new_object();
    json_object_object_add(audio, "format", json_object_new_string("pcm"));
    json_object_object_add(audio, "rate", json_object_new_int(state->ctx->audio_info.sample_rate));
    json_object_object_add(audio, "bits", json_object_new_int(state->ctx->audio_info.sample_bit));
    json_object_object_add(audio, "channel", json_object_new_int(state->ctx->audio_info.channels));
    json_object_object_add(audio, "codec", json_object_new_string(state->ctx->audio_info.audio_type));
    json_object_object_add(payload, "audio", audio);

    struct json_object* request = json_object_new_object();
    json_object_object_add(request, "model_name", json_object_new_string("bigmodel"));
    json_object_object_add(request, "enable_punc", json_object_new_boolean(true));
    json_object_object_add(payload, "request", request);

    const char* json_str = json_object_to_json_string(payload);

    // compressed_len = strlen(json_str) + strlen(json_str) / 5;
    // compressed = malloc(compressed_len);
    // volc_gzip_compress((const unsigned char*)json_str, strlen(json_str), &compressed, &compressed_len);
    // todo: free compressed

    compressed_len = strlen(json_str);
    compressed = json_str;

    volc_generate_message_header(headers, VOLC_FULL_CLIENT_REQUEST, VOLC_POS_SEQUENCE, VOLC_JSON, VOLC_NO_COMPRESSION);

    size_t message_size = sizeof(headers) + seq_len + payload_len + compressed_len;
    unsigned char* message = malloc(message_size + LWS_PRE);

    memcpy(message + dest_pos, headers, sizeof(headers));
    dest_pos += sizeof(headers);

    volc_int_to_bytes(state->seq, message + dest_pos);
    state->seq++;
    dest_pos += seq_len;

    volc_int_to_bytes(compressed_len, message + dest_pos);
    dest_pos += payload_len;

    memcpy(message + dest_pos, compressed, compressed_len);

    len = lws_write(state->wsi, message + LWS_PRE, message_size, LWS_WRITE_BINARY);
    if (len < message_size)
        AI_INFO("volc_send_initial_request: len < message_size");

    AI_INFO("asr_volc send initial request:%s\n", json_str);

    free(message);
    // free(compressed);
    json_object_put(payload);
    lws_callback_on_writable(state->wsi);
}

static void volc_send_audio_data(struct volc_lws_state* state)
{
    size_t compressed_len;
    unsigned char* message;
    char* compressed;
    char* frame_buffer;
    int payload_len = 4;
    size_t message_size;
    int buffer_size;
    int seq_len = 4;
    char headers[4];
    int dest_pos = LWS_PRE;
    int len;

    int frame_size = state->ctx->audio_info.sample_rate * state->ctx->audio_info.channels * state->ctx->audio_info.sample_bit / 8 / 10;
    if (frame_size == 0)
        frame_size = 3200;

    if (state->ctx->is_finished)
        buffer_size = 0;
    else {
        buffer_size = ai_ring_buffer_num_items(&state->buffer);
        if (buffer_size <= 0)
            return;
    }

    if (buffer_size < frame_size)
        return;

    frame_buffer = (char*)malloc(frame_size);
    if (state->ctx->is_finished) {
        memset(frame_buffer, 0, frame_size);
        usleep(VOLC_SILENCE_TIMEOUT);
    } else
        ai_ring_buffer_dequeue_arr(&state->buffer, frame_buffer, frame_size);

    // compressed_len = strlen(json_str) + strlen(json_str) / 5;
    // compressed = malloc(compressed_len);
    // volc_gzip_compress((const unsigned char *)frame_buffer, frame_size, &compressed, &compressed_len);
    // todo: free compressed

    compressed_len = frame_size;
    compressed = frame_buffer;

    char message_type_specific_flags = state->ctx->is_finished ? VOLC_NEG_WITH_SEQUENCE : VOLC_POS_SEQUENCE;
    volc_generate_message_header(headers, VOLC_AUDIO_ONLY_REQUEST, message_type_specific_flags, VOLC_JSON, VOLC_NO_COMPRESSION);

    message_size = sizeof(headers) + seq_len + payload_len + compressed_len;
    message = malloc(message_size + LWS_PRE);

    memcpy(message + dest_pos, headers, sizeof(headers));
    dest_pos += sizeof(headers);

    if (state->ctx->is_finished)
        state->seq = -state->seq;
    volc_int_to_bytes(state->seq, message + dest_pos);
    state->seq++;
    dest_pos += seq_len;

    volc_int_to_bytes(compressed_len, message + dest_pos);
    dest_pos += payload_len;

    memcpy(message + dest_pos, compressed, compressed_len);

    AI_INFO("asr_volc Write message of length %zu\n", message_size);
    len = lws_write(state->wsi, message + LWS_PRE, message_size, LWS_WRITE_BINARY);

    if (len < message_size)
        AI_INFO("volc_callback_bigasr: len < message_size");

    free(frame_buffer);
    free(message);
    lws_callback_on_writable(state->wsi);
}

static int volc_callback_bigasr(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len);

static struct lws_protocols asr_protocols[] = {
    { VOLC_CLIENT_PROTOCOL_NAME, volc_callback_bigasr, 0, 0 },
    { NULL, NULL, 0, 0 }
};

static int volc_callback_bigasr(struct lws* wsi, enum lws_callback_reasons reason, void* user, void* in, size_t len)
{
    struct volc_lws_state* state = (struct volc_lws_state*)user;
    int remaining = 0;
    int code = 0;
    int ret;

    AI_INFO("websocket_callback reason: %d", reason);
    
    switch (reason) {
    case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
        AI_INFO("asr_volc Add header\n");
        unsigned char** headers = (unsigned char**)in;
        unsigned char* end = (*headers) + len;
        volc_generate_uuid(state->connect_id, sizeof(state->connect_id));

        ret = lws_add_http_header_by_name(wsi,
            (unsigned char*)"X-Api-App-Key:",
            (unsigned char*)state->ctx->app_id,
            strlen(state->ctx->app_id),
            headers, end);
        if (ret < 0)
            AI_INFO("Add X-Api-App-Key failed\n");

        ret = lws_add_http_header_by_name(wsi,
            (unsigned char*)"X-Api-Access-Key:",
            (unsigned char*)state->ctx->app_key,
            strlen(state->ctx->app_key),
            headers, end);
        if (ret < 0)
            AI_INFO("Add X-Api-Access-Key failed\n");

        ret = lws_add_http_header_by_name(wsi,
            (unsigned char*)"X-Api-Resource-Id:",
            (unsigned char*)"volc.bigasr.sauc.duration",
            strlen("volc.bigasr.sauc.duration"),
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
        AI_INFO("asr_volc Connected to server\n");
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
        // AI_INFO("asr_volc Received message of length %zu %d\n", len, lws_is_final_fragment(wsi));
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
        if (!lws_is_final_fragment(state->wsi))
            break;

        volc_response_result result;
        int frame_size = state->recv_buf_ptr - state->recv_buf;
        volc_parse_response(state->recv_buf, frame_size, &result);
        state->recv_buf_ptr = state->recv_buf;

        if (state->ctx->cb) {
            voice_result_t cb_result;
            if (result.text) {
                cb_result.result = result.text;
                state->ctx->cb(voice_event_result, &cb_result, state->ctx->cookie);
                if (result.completed)
                    state->ctx->cb(voice_event_complete, NULL, state->ctx->cookie);
                free(result.text);
            } else if (result.completed) {
                state->ctx->cb(voice_event_complete, NULL, state->ctx->cookie);
            } else if (result.error_msg && result.code != 0) {
                cb_result.error_code = voice_error_unknown;
                cb_result.result = NULL;
                state->ctx->cb(voice_event_error, &cb_result, state->ctx->cookie);
            } else {
                AI_INFO("asr_volc error result!");
            }
        }
        break;
    case LWS_CALLBACK_CLIENT_WRITEABLE:
        // AI_INFO("asr_volc Write message of length %zu\n", len);
        if (state->seq == 1)
            volc_send_initial_request(state);
        else
            volc_send_audio_data(state);
        break;
    case LWS_CALLBACK_CLOSED:
        AI_INFO("asr_volc Connection closed\n");
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        AI_INFO("asr_volc Connection error: %s\n", in ? (char*)in : "(no error information)");
        if (state->ctx->cb) {
            voice_result_t cb_result;
            cb_result.error_code = voice_error_network;
            cb_result.result = NULL;
            state->ctx->cb(voice_event_error, &cb_result, state->ctx->cookie);
            lws_cancel_service(state->lws_ctx);
        }
        break;
    case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP:
        AI_INFO("asr_volc established http\n");
        break;
    case LWS_CALLBACK_CLIENT_HTTP_DROP_PROTOCOL:
        AI_INFO("asr_volc drop protocol\n");
        break;
    case LWS_CALLBACK_CLIENT_CLOSED:
        code = lws_http_client_http_response(wsi);
        AI_INFO("asr_volc Connection closed: code=%d, msg=%s\n", code, in ? (char*)in : "(no error information)");
        break;
    case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
        AI_INFO("asr_volc Peer initiated close\n");
        if (len >= 2) {
            char* codeBuf = (char*)in;
            code = code | (0xff & codeBuf[0]) << 8;
            code = code | (0xff & codeBuf[1]);

            AI_INFO("asr_volc Peer close reason:%d\n", code);
        }
        break;
    default:
        AI_INFO("asr_volc Default reason %d \n", reason);
        break;
    }

    return 0;
}

static struct lws_context* volc_create_websocket_connection(volc_context_t* ctx)
{
    struct lws_context_creation_info info;
    struct lws_context* context;

    memset(&info, 0, sizeof(info));
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = asr_protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;

    context = lws_create_context(&info);
    if (!context) {
        AI_INFO("Failed to create context\n");
        return NULL;
    }

    // lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO | LLL_DEBUG, NULL);

    ctx->state = (struct volc_lws_state*)calloc(1, sizeof(struct volc_lws_state));
    if (!ctx->state) {
        perror("calloc failed");
        lws_context_destroy(context);
        return NULL;
    }
    ctx->state->ctx = ctx;
    ctx->state->lws_ctx = context;
    ctx->state->seq = 1;

    struct lws_client_connect_info ccinfo = { 0 };
    ccinfo.context = context;
    ccinfo.address = VOLC_HOST; // 121.228.130.195
    ccinfo.port = 443;
    ccinfo.path = VOLC_PATH;
    ccinfo.host = VOLC_HOST;
    ccinfo.origin = VOLC_HOST;
    ccinfo.protocol = asr_protocols[0].name;
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

    AI_INFO("asr_volc Connected to server: %s\n", VOLC_URL);

    return context;
}

static void volc_uv_handle_close(uv_handle_t* handle, void* arg)
{
    if (uv_is_active(handle) && !uv_is_closing(handle))
        uv_close(handle, NULL);
}

static int volc_close_handle(void* engine, int sync)
{
    volc_context_t* ctx = (volc_context_t*)engine;

    if (engine == NULL)
        return -EINVAL;

    uv_walk(&ctx->loop, volc_uv_handle_close, NULL);

    if (sync) {
        while (uv_loop_alive(&ctx->loop)) {
            uv_run(&ctx->loop, UV_RUN_ONCE);
        }
    }

    return 0;
}

__attribute__((used)) static voice_error_t get_errcode(int code)
{
    voice_err_code_t* err_code = NULL;

    for (int i = 0; (!err_code || err_code->str_code != NULL); i++) {
        err_code = &errcode_map[i];
        if (err_code->pb_code == code)
            return err_code->voice_code;
    }

    return voice_error_success;
}

static void volc_init_data(volc_context_t* ctx)
{
    ctx->is_running = false;
    ctx->is_finished = true;
    ctx->is_closed = false;
    ctx->audio_info.sample_rate = 16000;
    ctx->audio_info.channels = 1;
    ctx->audio_info.sample_bit = 16;
    strlcpy(ctx->audio_info.audio_type, "raw", sizeof(ctx->audio_info.audio_type));
}

static void volc_destroy_lws_state(volc_context_t* ctx)
{
    if (ctx->state) {
        if (ctx->state->buffer.buffer) {
            free(ctx->state->buffer.buffer);
            ctx->state->buffer.buffer = NULL;
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

static void volc_destroy_data(volc_context_t* ctx)
{
    sem_destroy(&ctx->sem);

    volc_destroy_lws_state(ctx);

    if (ctx->env_params) {
        free(ctx->env_params);
        ctx->env_params = NULL;
    }

    if (ctx->app_id) {
        free(ctx->app_id);
        ctx->app_id = NULL;
    }

    if (ctx->app_key) {
        free(ctx->app_key);
        ctx->app_key = NULL;
    }

    free(ctx);
}

static void* volc_uvloop_thread(void* arg)
{
    volc_context_t* ctx = (volc_context_t*)arg;
    int ret;

    volc_init_data(ctx);

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
        AI_INFO("asr_asyncq_init:%p", ctx->asyncq);
    }

    AI_INFO("[%s][%d] asr_running:%d\n", __func__, __LINE__, uv_loop_alive(&ctx->loop));

    while (uv_loop_alive(&ctx->loop) && !ctx->is_closed) {
        ret = uv_run(&ctx->loop, UV_RUN_NOWAIT);
        if (ret == 0)
            break;

        if (!ctx->is_finished && ctx->state && ctx->state->lws_ctx) {
            ret = lws_service(ctx->state->lws_ctx, -1);
            if (ret < 0) {
                AI_INFO("asr_service failed\n");
                if (ctx->cb) {
                    voice_result_t cb_result;
                    cb_result.error_code = voice_error_network;
                    cb_result.result = NULL;
                    ctx->cb(voice_event_error, &cb_result, ctx->cookie);
                }
                break;
            }
        } else if (ctx->is_finished && ctx->state && ctx->state->lws_ctx) {
            lws_context_destroy(ctx->state->lws_ctx);
            ctx->state->lws_ctx = NULL;
            volc_destroy_lws_state(ctx);
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

    volc_close_handle(ctx, 1);
    uv_stop(&ctx->loop);

out:
    if (ctx->asyncq) {
        free(ctx->asyncq);
        ctx->asyncq = NULL;
    }
    ret = uv_loop_close(&ctx->loop);
    ctx->is_running = false;
    volc_destroy_data(ctx);
    AI_INFO("[%s][%d] asr_thread_out:%d\n", __func__, __LINE__, ret);

    return NULL;
}

static int volc_create_thread(volc_context_t* ctx)
{
    struct sched_param param;
    pthread_attr_t attr;
    int ret;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16384);
    param.sched_priority = 110;
    pthread_attr_setschedparam(&attr, &param);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&ctx->thread, &attr, volc_uvloop_thread, ctx);
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

static int volc_init(void* engine, const voice_init_params_t* param)
{
    volc_context_t* ctx = (volc_context_t*)engine;
    int ret;

    if (engine == NULL || param == NULL) {
        free(ctx);
        return -EINVAL;
    }

    sem_init(&ctx->sem, 0, 0);

    ctx->uvasyncq_cb = param->cb;
    ctx->opaque = param->opaque;
    if (param->app_id) {
        ctx->app_id = (char*)malloc(strlen(param->app_id) + 1);
        strlcpy(ctx->app_id, param->app_id, strlen(param->app_id) + 1);
    }
    if (param->app_key) {
        ctx->app_key = (char*)malloc(strlen(param->app_key) + 1);
        strlcpy(ctx->app_key, param->app_key, strlen(param->app_key) + 1);
    }
    ctx->is_running = false;
    ret = volc_create_thread(ctx);

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

    AI_INFO("asr_volc_init");
    return ret;
}

static int volc_uninit(void* engine)
{
    volc_context_t* ctx = (volc_context_t*)engine;

    if (engine == NULL)
        return -EINVAL;

    ctx->cb = NULL;
    ctx->cookie = NULL;
    ctx->is_closed = true;
    // ret = volc_close_handle(engine, 0);

    return 0;
}

static int volc_event_cb(void* engine, voice_callback_t callback, void* cookie)
{
    volc_context_t* ctx = (volc_context_t*)engine;

    if (engine == NULL)
        return -EINVAL;

    ctx->cb = callback;
    ctx->cookie = cookie;
    return 0;
}

static int volc_start(void* engine, const voice_audio_info_t* audio_info)
{
    volc_context_t* ctx = (volc_context_t*)engine;
    struct lws_context* context;

    if (engine == NULL)
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

    context = volc_create_websocket_connection(ctx);
    if (context == NULL) {
        AI_INFO("asr_create_connect failed\n");
        return -ENOTCONN;
    }

    ctx->is_finished = false;

    return 0;
}

static int volc_write_audio(void* engine, const char* data, int len)
{
    volc_context_t* ctx = (volc_context_t*)engine;

    if (engine == NULL || data == NULL || len <= 0 || len > VOLC_BUFFER_MAX_SIZE)
        return -EINVAL;

    if (ctx->state == NULL || ctx->state->lws_ctx == NULL) {
        AI_INFO("asr_volc_write_audio: state is NULL\n");
        return -EINVAL;
    }

    if (ctx->is_finished)
        return -EPERM;

    if (ctx->state->buffer.buffer == NULL) {
        AI_INFO("asr_volc init ring buffer\n");
        char* buffer = (char*)malloc(VOLC_BUFFER_MAX_SIZE);
        ai_ring_buffer_init(&ctx->state->buffer, buffer, VOLC_BUFFER_MAX_SIZE);
    }

    if (ai_ring_buffer_is_full(&ctx->state->buffer)) {
        AI_INFO("asr_volc ring buffer is full\n");
        ai_ring_buffer_clear_arr(&ctx->state->buffer, len);
    }
    ai_ring_buffer_queue_arr(&ctx->state->buffer, data, len);
    lws_callback_on_writable(ctx->state->wsi);

    return 0;
}

static int volc_finish(void* engine)
{
    volc_context_t* ctx = (volc_context_t*)engine;

    if (engine == NULL)
        return -EINVAL;

    // volc_send_audio_data(ctx->state);
    ctx->is_finished = true;

    if (ctx->state == NULL) {
        AI_INFO("asr_volc_finish: state is NULL\n");
        return -EINVAL;
    }

    if (ctx->state->buffer.buffer) {
        free(ctx->state->buffer.buffer);
        ctx->state->buffer.buffer = NULL;
    }

    if (ctx->state->recv_buf)
        ctx->state->recv_buf_ptr = ctx->state->recv_buf;

    lws_callback_on_writable(ctx->state->wsi);

    return 0;
}

static int volc_cancel(void* engine)
{
    if (engine == NULL)
        return -EINVAL;

    return 0;
}

static voice_env_params_t* volc_get_env_params(void* engine)
{
    volc_context_t* ctx = (volc_context_t*)engine;
    voice_env_params_t* env_params;

    if (engine == NULL)
        return NULL;

    if (ctx->env_params) {
        AI_INFO("volc_get_env_params exist");
        return ctx->env_params;
    }

    env_params = (voice_env_params_t*)malloc(sizeof(voice_env_params_t));
    env_params->format = "format=s16le:sample_rate=16000:ch_layout=mono";
    env_params->force_format = 1;
    env_params->loop = &ctx->loop;
    env_params->asyncq = ctx->asyncq;
    ctx->env_params = env_params;

    AI_INFO("volc_get_env_params");

    return env_params;
}

voice_plugin_t volc_asr_engine_plugin = {
    .name = "volc_asr",
    .priv_size = sizeof(volc_context_t),
    .init = volc_init,
    .uninit = volc_uninit,
    .event_cb = volc_event_cb,
    .start = volc_start,
    .write_audio = volc_write_audio,
    .finish = volc_finish,
    .cancel = volc_cancel,
    .get_env = volc_get_env_params,
};
