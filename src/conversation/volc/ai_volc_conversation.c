/****************************************************************************
 * frameworks/ai/src/conversation/volc/ai_volc_conversation.c
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

#include <errno.h>
#include <json_object.h>
#include <json_tokener.h>
#include <libwebsockets.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>
#include <uv_async_queue.h>

#include "ai_common.h"
#include "ai_conversation_plugin.h"
#include "ai_ring_buffer.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

#define VOLC_CONVERSATION_URL "ai-gateway.vei.volces.com"
#define VOLC_CONVERSATION_PATH "/v1/realtime"
#define VOLC_CONVERSATION_PORT 443
#define VOLC_BUFFER_SIZE 64 * 1024

typedef enum {
    VOLC_STATE_DISCONNECTED,
    VOLC_STATE_CONNECTING,
    VOLC_STATE_CONNECTED,
    VOLC_STATE_SESSION_CREATED,
    VOLC_STATE_LISTENING,
    VOLC_STATE_PROCESSING,
    VOLC_STATE_SPEAKING,
    VOLC_STATE_ERROR
} volc_conversation_state_t;

typedef struct volc_conversation_engine {
    // WebSocket connection
    struct lws_context* lws_context;
    struct lws* wsi;
    
    // State management
    volc_conversation_state_t state;
    conversation_engine_callback_t event_callback;
    void* event_cookie;
    
    // Configuration
    conversation_engine_init_params_t config;
    
    // Send buffer
    ai_ring_buffer_t send_buffer;
    char* send_buffer_data;
    
    // Session data
    char* session_id;
    char* current_response_id;
    
    // Connection state
    int is_connected;
    int is_connecting;
    
    // Environment
    conversation_engine_env_params_t env;
    
} volc_conversation_engine_t;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int volc_conversation_websocket_callback(struct lws* wsi, enum lws_callback_reasons reason,
                                               void* user, void* in, size_t len);
static int volc_conversation_send_json_message(volc_conversation_engine_t* engine, json_object* json_obj);
static int volc_conversation_process_server_message(volc_conversation_engine_t* engine, const char* message);
static void volc_conversation_send_event(volc_conversation_engine_t* engine, 
                                        conversation_engine_event_t event,
                                        const char* text,
                                        const void* audio_data,
                                        int audio_length,
                                        conversation_engine_error_t error_code);
static char* base64_encode(const unsigned char* data, size_t input_length);
static unsigned char* base64_decode(const char* data, size_t input_length, size_t* output_length);

/****************************************************************************
 * WebSocket Protocol Implementation
 ****************************************************************************/

static struct lws_protocols volc_conversation_protocols[] = {
    {
        .name = "volc-conversation",
        .callback = volc_conversation_websocket_callback,
        .per_session_data_size = 0,
        .rx_buffer_size = VOLC_BUFFER_SIZE,
    },
    { NULL, NULL, 0, 0 }
};

static int volc_conversation_websocket_callback(struct lws* wsi, enum lws_callback_reasons reason,
                                               void* user, void* in, size_t len)
{
    volc_conversation_engine_t* engine = (volc_conversation_engine_t*)lws_context_user(lws_get_context(wsi));
    
    if (!engine) {
        return -1;
    }
    
    switch (reason) {
        case LWS_CALLBACK_CLIENT_ESTABLISHED:
            AI_INFO("WebSocket connected to VolcEngine");
            engine->is_connected = 1;
            engine->is_connecting = 0;
            engine->wsi = wsi;
            engine->state = VOLC_STATE_CONNECTED;
                    volc_conversation_send_event(engine, conversation_engine_event_start,
                                     NULL, conversation_engine_error_success);
            break;
            
        case LWS_CALLBACK_CLIENT_RECEIVE:
            if (len > 0) {
                char* message = malloc(len + 1);
                memcpy(message, in, len);
                message[len] = '\0';
                
                AI_INFO("Received: %.*s", (int)len, message);
                volc_conversation_process_server_message(engine, message);
                free(message);
            }
            break;
            
        case LWS_CALLBACK_CLIENT_WRITEABLE:
            if (ai_ring_buffer_num_items(&engine->send_buffer) > 0) {
                size_t available = ai_ring_buffer_num_items(&engine->send_buffer);
                size_t to_send = available > VOLC_BUFFER_SIZE - LWS_PRE ? 
                                VOLC_BUFFER_SIZE - LWS_PRE : available;
                
                unsigned char* buffer = malloc(to_send + LWS_PRE);
                ai_ring_buffer_dequeue_arr(&engine->send_buffer, (char*)(buffer + LWS_PRE), to_send);
                
                int written = lws_write(wsi, buffer + LWS_PRE, to_send, LWS_WRITE_TEXT);
                free(buffer);
                
                if (written < 0) {
                    return -1;
                }
                
                if (ai_ring_buffer_num_items(&engine->send_buffer) > 0) {
                    lws_callback_on_writable(wsi);
                }
            }
            break;
            
        case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
            AI_INFO("WebSocket connection error: %s", in ? (char*)in : "Unknown error");
            engine->is_connected = 0;
            engine->is_connecting = 0;
            engine->state = VOLC_STATE_ERROR;
            volc_conversation_send_event(engine, conversation_engine_event_error, 
                                       in ? (char*)in : "Connection error", 
                                       conversation_engine_error_network);
            break;
            
        case LWS_CALLBACK_CLIENT_CLOSED:
            AI_INFO("WebSocket connection closed");
            engine->is_connected = 0;
            engine->is_connecting = 0;
            engine->wsi = NULL;
            engine->state = VOLC_STATE_DISCONNECTED;
                    volc_conversation_send_event(engine, conversation_engine_event_stop,
                                     NULL, conversation_engine_error_success);
            break;
            
        case LWS_CALLBACK_WSI_DESTROY:
            engine->wsi = NULL;
            break;
            
        default:
            break;
    }
    
    return 0;
}

/****************************************************************************
 * JSON Message Processing
 ****************************************************************************/

static int volc_conversation_send_json_message(volc_conversation_engine_t* engine, json_object* json_obj)
{
    if (!engine || !engine->is_connected || !json_obj) {
        return -EINVAL;
    }
    
    const char* json_string = json_object_to_json_string(json_obj);
    size_t json_len = strlen(json_string);
    
    AI_INFO("Sending: %s", json_string);
    
    if (ai_ring_buffer_capacity(&engine->send_buffer) - ai_ring_buffer_num_items(&engine->send_buffer) < json_len) {
        AI_INFO("Send buffer full");
        return -ENOMEM;
    }
    
    ai_ring_buffer_queue_arr(&engine->send_buffer, json_string, json_len);
    lws_callback_on_writable(engine->wsi);
    
    return 0;
}

static int volc_conversation_process_server_message(volc_conversation_engine_t* engine, const char* message)
{
    json_object* json = json_tokener_parse(message);
    if (!json) {
        AI_INFO("Failed to parse JSON message");
        return -1;
    }
    
    json_object* type_obj;
    if (!json_object_object_get_ex(json, "type", &type_obj)) {
        json_object_put(json);
        return -1;
    }
    
    const char* type = json_object_get_string(type_obj);
    
    if (strcmp(type, "session.created") == 0) {
        json_object* session_obj;
        if (json_object_object_get_ex(json, "session", &session_obj)) {
            json_object* id_obj;
            if (json_object_object_get_ex(session_obj, "id", &id_obj)) {
                const char* session_id = json_object_get_string(id_obj);
                if (engine->session_id) {
                    free(engine->session_id);
                }
                engine->session_id = strdup(session_id);
            }
        }
        
        engine->state = VOLC_STATE_SESSION_CREATED;
        volc_conversation_send_event(engine, conversation_engine_event_start, 
                                   engine->session_id, conversation_engine_error_success);
        
    } else if (strcmp(type, "input_audio_buffer.committed") == 0) {
        engine->state = VOLC_STATE_PROCESSING;
                volc_conversation_send_event(engine, conversation_engine_event_start,
                                     NULL, conversation_engine_error_success);
        
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        json_object* transcript_obj;
        if (json_object_object_get_ex(json, "transcript", &transcript_obj)) {
            const char* transcript = json_object_get_string(transcript_obj);
            volc_conversation_send_event(engine, conversation_engine_event_result, 
                                       transcript, conversation_engine_error_success);
        }
        
    } else if (strcmp(type, "response.created") == 0) {
        json_object* response_obj;
        if (json_object_object_get_ex(json, "response", &response_obj)) {
            json_object* id_obj;
            if (json_object_object_get_ex(response_obj, "id", &id_obj)) {
                const char* response_id = json_object_get_string(id_obj);
                if (engine->current_response_id) {
                    free(engine->current_response_id);
                }
                engine->current_response_id = strdup(response_id);
            }
        }
        
        engine->state = VOLC_STATE_SPEAKING;
        volc_conversation_send_event(engine, conversation_engine_event_start, 
                                   NULL, conversation_engine_error_success);
        
    } else if (strcmp(type, "response.audio.delta") == 0) {
        json_object* delta_obj;
        if (json_object_object_get_ex(json, "delta", &delta_obj)) {
            const char* audio_b64 = json_object_get_string(delta_obj);
            
            size_t audio_len;
            unsigned char* audio_data = base64_decode(audio_b64, strlen(audio_b64), &audio_len);
            
            if (audio_data) {
                volc_conversation_send_event(engine, conversation_engine_event_result, 
                                           (char*)audio_data, conversation_engine_error_success);
                free(audio_data);
            }
        }
        
    } else if (strcmp(type, "response.audio_transcript.delta") == 0) {
        json_object* delta_obj;
        if (json_object_object_get_ex(json, "delta", &delta_obj)) {
            const char* text_delta = json_object_get_string(delta_obj);
            volc_conversation_send_event(engine, conversation_engine_event_result, 
                                       text_delta, conversation_engine_error_success);
        }
        
    } else if (strcmp(type, "response.done") == 0) {
        engine->state = VOLC_STATE_SESSION_CREATED;
        volc_conversation_send_event(engine, conversation_engine_event_complete, 
                                   NULL, conversation_engine_error_success);
        
        if (engine->current_response_id) {
            free(engine->current_response_id);
            engine->current_response_id = NULL;
        }
        
    } else if (strcmp(type, "error") == 0) {
        json_object* error_obj;
        const char* error_message = "Unknown error";
        if (json_object_object_get_ex(json, "error", &error_obj)) {
            json_object* message_obj;
            if (json_object_object_get_ex(error_obj, "message", &message_obj)) {
                error_message = json_object_get_string(message_obj);
            }
        }
        
        engine->state = VOLC_STATE_ERROR;
        volc_conversation_send_event(engine, conversation_engine_event_error, 
                                   error_message, conversation_engine_error_network);
    }
    
    json_object_put(json);
    return 0;
}

/****************************************************************************
 * Utility Functions
 ****************************************************************************/

static void volc_conversation_send_event(volc_conversation_engine_t* engine, 
                                        conversation_engine_event_t event,
                                        const char* result,
                                        conversation_engine_error_t error_code)
{
    if (!engine || !engine->event_callback) {
        return;
    }
    
    conversation_engine_result_t engine_result = {
        .result = result,
        .len = result ? strlen(result) : 0,
        .error_code = error_code
    };
    
    engine->event_callback(event, &engine_result, engine->event_cookie);
}

// Base64编码实现（简化版）
static char* base64_encode(const unsigned char* data, size_t input_length)
{
    static const char encoding_table[] = {
        'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
        'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
        'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
        'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
        'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
        'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
        'w', 'x', 'y', 'z', '0', '1', '2', '3',
        '4', '5', '6', '7', '8', '9', '+', '/'
    };
    
    size_t output_length = 4 * ((input_length + 2) / 3);
    char* encoded_data = malloc(output_length + 1);
    if (!encoded_data) return NULL;
    
    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;
        
        uint32_t triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
        
        encoded_data[j++] = encoding_table[(triple >> 3 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 2 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 1 * 6) & 0x3F];
        encoded_data[j++] = encoding_table[(triple >> 0 * 6) & 0x3F];
    }
    
    static const int mod_table[] = {0, 2, 1};
    for (int i = 0; i < mod_table[input_length % 3]; i++)
        encoded_data[output_length - 1 - i] = '=';
    
    encoded_data[output_length] = '\0';
    return encoded_data;
}

// Base64解码实现（简化版）
static unsigned char* base64_decode(const char* data, size_t input_length, size_t* output_length)
{
    if (input_length % 4 != 0) return NULL;
    
    *output_length = input_length / 4 * 3;
    if (data[input_length - 1] == '=') (*output_length)--;
    if (data[input_length - 2] == '=') (*output_length)--;
    
    // 简化实现，实际项目中应使用更完整的解码
    unsigned char* decoded_data = malloc(*output_length);
    if (!decoded_data) return NULL;
    
    // 这里应该实现完整的base64解码逻辑
    // 为简化起见，暂时使用占位实现
    memset(decoded_data, 0, *output_length);
    
    return decoded_data;
}

/****************************************************************************
 * Plugin Interface Implementation
 ****************************************************************************/

static int volc_conversation_init(void* engine, const conversation_engine_init_params_t* param)
{
    volc_conversation_engine_t* volc_engine = (volc_conversation_engine_t*)engine;
    
    if (!volc_engine || !param) {
        return -EINVAL;
    }
    
    AI_INFO("Initializing VolcEngine conversation");
    
    // 复制配置
    memcpy(&volc_engine->config, param, sizeof(conversation_engine_init_params_t));
    
    // 初始化发送缓冲区
    volc_engine->send_buffer_data = malloc(VOLC_BUFFER_SIZE);
    if (!volc_engine->send_buffer_data) {
        return -ENOMEM;
    }
    ai_ring_buffer_init(&volc_engine->send_buffer, volc_engine->send_buffer_data, VOLC_BUFFER_SIZE);
    
    // 设置环境参数
    volc_engine->env.loop = param->loop;
    volc_engine->env.format = "pcm16";
    volc_engine->env.force_format = 1;
    volc_engine->env.asyncq = param->asyncq;
    
    volc_engine->state = VOLC_STATE_DISCONNECTED;
    
    AI_INFO("VolcEngine conversation initialized");
    return 0;
}

static int volc_conversation_uninit(void* engine)
{
    volc_conversation_engine_t* volc_engine = (volc_conversation_engine_t*)engine;
    
    if (!volc_engine) {
        return -EINVAL;
    }
    
    AI_INFO("Uninitializing VolcEngine conversation");
    
    // 关闭连接
    if (volc_engine->wsi) {
        lws_close_reason(volc_engine->wsi, LWS_CLOSE_STATUS_NORMAL, NULL, 0);
        volc_engine->wsi = NULL;
    }
    
    if (volc_engine->lws_context) {
        lws_context_destroy(volc_engine->lws_context);
        volc_engine->lws_context = NULL;
    }
    
    // 清理内存
    if (volc_engine->send_buffer_data) {
        free(volc_engine->send_buffer_data);
    }
    if (volc_engine->session_id) {
        free(volc_engine->session_id);
    }
    if (volc_engine->current_response_id) {
        free(volc_engine->current_response_id);
    }
    
    AI_INFO("VolcEngine conversation uninitialized");
    return 0;
}

static int volc_conversation_event_cb(void* engine, conversation_engine_callback_t callback, void* cookie)
{
    volc_conversation_engine_t* volc_engine = (volc_conversation_engine_t*)engine;
    
    if (!volc_engine) {
        return -EINVAL;
    }
    
    volc_engine->event_callback = callback;
    volc_engine->event_cookie = cookie;
    
    return 0;
}

static int volc_conversation_start(void* engine, const conversation_engine_audio_info_t* audio_info)
{
    volc_conversation_engine_t* volc_engine = (volc_conversation_engine_t*)engine;
    
    if (!volc_engine) {
        return -EINVAL;
    }
    
    if (volc_engine->is_connected || volc_engine->is_connecting) {
        return 0; // 已经连接或正在连接
    }
    
    AI_INFO("Starting VolcEngine conversation connection");
    
    // 创建WebSocket上下文
    struct lws_context_creation_info info;
    memset(&info, 0, sizeof(info));
    
    info.port = CONTEXT_PORT_NO_LISTEN;
    info.protocols = volc_conversation_protocols;
    info.gid = -1;
    info.uid = -1;
    info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
    info.user = volc_engine;
    
    volc_engine->lws_context = lws_create_context(&info);
    if (!volc_engine->lws_context) {
        AI_INFO("Failed to create WebSocket context");
        return -1;
    }
    
    // 构建连接信息
    struct lws_client_connect_info ccinfo;
    memset(&ccinfo, 0, sizeof(ccinfo));
    
    ccinfo.context = volc_engine->lws_context;
    ccinfo.address = VOLC_CONVERSATION_URL;
    ccinfo.port = VOLC_CONVERSATION_PORT;
    ccinfo.path = VOLC_CONVERSATION_PATH "?model=AG-voice-chat-agent";
    ccinfo.host = VOLC_CONVERSATION_URL;
    ccinfo.origin = VOLC_CONVERSATION_URL;
    ccinfo.protocol = volc_conversation_protocols[0].name;
    ccinfo.ssl_connection = LCCSCF_USE_SSL | LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
    
    // 设置认证头
    static const char* headers[] = {
        "Authorization:", NULL,
        NULL
    };
    
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", volc_engine->config.app_key);
    headers[1] = auth_header;
    
    ccinfo.extra_headers = headers;
    
    // 发起连接
    volc_engine->wsi = lws_client_connect_via_info(&ccinfo);
    if (!volc_engine->wsi) {
        AI_INFO("Failed to initiate WebSocket connection");
        lws_context_destroy(volc_engine->lws_context);
        volc_engine->lws_context = NULL;
        return -1;
    }
    
    volc_engine->is_connecting = 1;
    volc_engine->state = VOLC_STATE_CONNECTING;
    
    AI_INFO("WebSocket connection initiated");
    return 0;
}

static int volc_conversation_write_audio(void* engine, const char* data, int len)
{
    volc_conversation_engine_t* volc_engine = (volc_conversation_engine_t*)engine;
    
    if (!volc_engine || !data || len <= 0 || !volc_engine->is_connected) {
        return -EINVAL;
    }
    
    // Base64编码音频数据
    char* audio_b64 = base64_encode((const unsigned char*)data, len);
    if (!audio_b64) {
        return -ENOMEM;
    }
    
    // 构建JSON消息
    json_object* json = json_object_new_object();
    json_object_object_add(json, "type", json_object_new_string("input_audio_buffer.append"));
    json_object_object_add(json, "audio", json_object_new_string(audio_b64));
    
    int ret = volc_conversation_send_json_message(volc_engine, json);
    
    json_object_put(json);
    free(audio_b64);
    
    if (ret == 0 && volc_engine->state == VOLC_STATE_SESSION_CREATED) {
        volc_engine->state = VOLC_STATE_LISTENING;
        volc_conversation_send_event(volc_engine, conversation_engine_event_start, 
                                   NULL, conversation_engine_error_success);
    }
    
    return ret;
}

static int volc_conversation_finish(void* engine)
{
    volc_conversation_engine_t* volc_engine = (volc_conversation_engine_t*)engine;
    
    if (!volc_engine || !volc_engine->is_connected) {
        return -EINVAL;
    }
    
    // 提交音频缓冲区
    json_object* commit_json = json_object_new_object();
    json_object_object_add(commit_json, "type", json_object_new_string("input_audio_buffer.commit"));
    
    int ret = volc_conversation_send_json_message(volc_engine, commit_json);
    json_object_put(commit_json);
    
    if (ret < 0) {
        return ret;
    }
    
    // 请求响应
    json_object* response_json = json_object_new_object();
    json_object_object_add(response_json, "type", json_object_new_string("response.create"));
    
    ret = volc_conversation_send_json_message(volc_engine, response_json);
    json_object_put(response_json);
    
    return ret;
}

static int volc_conversation_cancel(void* engine)
{
    volc_conversation_engine_t* volc_engine = (volc_conversation_engine_t*)engine;
    
    if (!volc_engine || !volc_engine->is_connected || !volc_engine->current_response_id) {
        return -EINVAL;
    }
    
    // 取消当前响应
    json_object* json = json_object_new_object();
    json_object_object_add(json, "type", json_object_new_string("response.cancel"));
    
    int ret = volc_conversation_send_json_message(volc_engine, json);
    json_object_put(json);
    
    return ret;
}

static conversation_engine_env_params_t* volc_conversation_get_env(void* engine)
{
    volc_conversation_engine_t* volc_engine = (volc_conversation_engine_t*)engine;
    
    if (!volc_engine) {
        return NULL;
    }
    
    return &volc_engine->env;
}

/****************************************************************************
 * Plugin Definition
 ****************************************************************************/

conversation_engine_plugin_t volc_conversation_engine_plugin = {
    .name = "volc_conversation",
    .priv_size = sizeof(volc_conversation_engine_t),
    .init = volc_conversation_init,
    .uninit = volc_conversation_uninit,
    .event_cb = volc_conversation_event_cb,
    .start = volc_conversation_start,
    .write_audio = volc_conversation_write_audio,
    .finish = volc_conversation_finish,
    .cancel = volc_conversation_cancel,
    .get_env = volc_conversation_get_env,
}; 