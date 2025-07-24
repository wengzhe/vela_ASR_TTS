/****************************************************************************
 * frameworks/ai/src/conversation/ai_conversation.c
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>
#include <uv_async_queue.h>

#include "ai_conversation.h"
#include "ai_common.h"
#include "ai_conversation_plugin.h"

#define CONVERSATION_DEFAULT_TIMEOUT 30000
#define CONVERSATION_MIN_TIMEOUT 5000
#define CONVERSATION_MAX_TIMEOUT 120000

/****************************************************************************
 * Private Types
 ****************************************************************************/

extern conversation_engine_plugin_t volc_conversation_engine_plugin;

typedef enum {
    CONVERSATION_STATE_INIT,
    CONVERSATION_STATE_CONNECTED,
    CONVERSATION_STATE_ACTIVE,
    CONVERSATION_STATE_CLOSED
} conversation_state_t;

typedef struct conversation_context {
    conversation_engine_plugin_t* plugin;
    void* engine;
    uv_loop_t* loop;
    uv_loop_t* user_loop;
    uv_async_queue_t* asyncq;
    uv_async_queue_t user_asyncq;
    conversation_callback_t cb;
    void* cookie;
    conversation_state_t state;
    int is_closed;
    conversation_engine_init_params_t engine_param;
} conversation_context_t;

typedef enum {
    CONVERSATION_MESSAGE_CREATE_ENGINE,
    CONVERSATION_MESSAGE_LISTENER,
    CONVERSATION_MESSAGE_START,
    CONVERSATION_MESSAGE_SEND_AUDIO,
    CONVERSATION_MESSAGE_FINISH,
    CONVERSATION_MESSAGE_CANCEL,
    CONVERSATION_MESSAGE_IS_BUSY,
    CONVERSATION_MESSAGE_CLOSE,
    CONVERSATION_MESSAGE_CB
} message_id_t;

typedef int (*message_handler_t)(void* message_data);

typedef struct message_s {
    message_id_t message_id;
    message_handler_t message_handler;
    void* message_data;
} message_t;

typedef struct message_data_listener_s {
    conversation_context_t* ctx;
    conversation_callback_t cb;
    void* cookie;
} message_data_listener_t;

typedef struct message_data_start_s {
    conversation_context_t* ctx;
    conversation_audio_info_t audio_info;
} message_data_start_t;

typedef struct message_data_send_audio_s {
    conversation_context_t* ctx;
    void* data;
    int length;
} message_data_send_audio_t;

typedef struct message_data_finish_s {
    conversation_context_t* ctx;
} message_data_finish_t;

typedef struct message_data_cancel_s {
    conversation_context_t* ctx;
} message_data_cancel_t;

typedef struct message_data_close_s {
    conversation_context_t* ctx;
} message_data_close_t;

typedef struct message_data_cb_s {
    conversation_context_t* ctx;
    conversation_event_t event;
    conversation_result_t result;
} message_data_cb_t;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static conversation_engine_plugin_t* conversation_get_plugin(conversation_engine_type engine_type);
static void conversation_async_cb(uv_async_queue_t* asyncq, void* data);
static void conversation_user_async_cb(uv_async_queue_t* asyncq, void* data);
static void conversation_engine_event_cb(conversation_engine_event_t event, 
                                        const conversation_engine_result_t* result, 
                                        void* cookie);

static int conversation_message_listener_handler(void* message_data);
static int conversation_message_start_handler(void* message_data);
static int conversation_message_send_audio_handler(void* message_data);
static int conversation_message_finish_handler(void* message_data);
static int conversation_message_cancel_handler(void* message_data);
static int conversation_message_close_handler(void* message_data);
static int conversation_message_cb_handler(void* message_data);

/****************************************************************************
 * Plugin Selection
 ****************************************************************************/

static conversation_engine_plugin_t* conversation_get_plugin(conversation_engine_type engine_type)
{
    switch (engine_type) {
        case conversation_engine_type_volc:
            return &volc_conversation_engine_plugin;
        default:
            AI_INFO("Unsupported conversation engine type: %d", engine_type);
            return NULL;
    }
}

/****************************************************************************
 * Async Queue Callbacks
 ****************************************************************************/

static void conversation_async_cb(uv_async_queue_t* asyncq, void* data)
{
    message_t* message = (message_t*)data;
    
    if (!message || !message->message_handler) {
        AI_INFO("Invalid message in conversation async callback");
        return;
    }
    
    message->message_handler(message->message_data);
    
    if (message->message_data) {
        free(message->message_data);
    }
    free(message);
}

static void conversation_user_async_cb(uv_async_queue_t* asyncq, void* data)
{
    message_t* message = (message_t*)data;
    
    if (!message || !message->message_handler) {
        AI_INFO("Invalid message in conversation user async callback");
        return;
    }
    
    message->message_handler(message->message_data);
    
    if (message->message_data) {
        free(message->message_data);
    }
    free(message);
}

/****************************************************************************
 * Engine Event Callback
 ****************************************************************************/

static void conversation_engine_event_cb(conversation_engine_event_t event, 
                                        const conversation_engine_result_t* result, 
                                        void* cookie)
{
    conversation_context_t* ctx = (conversation_context_t*)cookie;
    
    if (!ctx) {
        AI_INFO("Invalid context in engine event callback");
        return;
    }
    
    // 映射事件类型
    conversation_event_t user_event;
    switch (event) {
        case conversation_engine_event_connected:
            user_event = conversation_event_connected;
            break;
        case conversation_engine_event_session_created:
            user_event = conversation_event_session_created;
            break;
        case conversation_engine_event_listening:
            user_event = conversation_event_listening;
            break;
        case conversation_engine_event_processing:
            user_event = conversation_event_processing;
            break;
        case conversation_engine_event_speaking:
            user_event = conversation_event_speaking;
            break;
        case conversation_engine_event_user_transcript:
            user_event = conversation_event_user_transcript;
            break;
        case conversation_engine_event_response_audio:
            user_event = conversation_event_response_audio;
            break;
        case conversation_engine_event_response_text:
            user_event = conversation_event_response_text;
            break;
        case conversation_engine_event_complete:
            user_event = conversation_event_complete;
            break;
        case conversation_engine_event_error:
            user_event = conversation_event_error;
            break;
        case conversation_engine_event_disconnected:
            user_event = conversation_event_disconnected;
            break;
        default:
            user_event = conversation_event_unknown;
            break;
    }
    
    // 创建回调消息
    message_data_cb_t* cb_data = calloc(1, sizeof(message_data_cb_t));
    if (!cb_data) {
        AI_INFO("Failed to allocate callback message data");
        return;
    }
    
    cb_data->ctx = ctx;
    cb_data->event = user_event;
    
    // 复制结果数据
    if (result) {
        if (result->text) {
            cb_data->result.text = strdup(result->text);
        }
        if (result->audio_data && result->audio_length > 0) {
            cb_data->result.audio_data = malloc(result->audio_length);
            if (cb_data->result.audio_data) {
                memcpy((void*)cb_data->result.audio_data, result->audio_data, result->audio_length);
                cb_data->result.audio_length = result->audio_length;
            }
        }
        cb_data->result.duration = result->duration;
        
        // 映射错误码
        switch (result->error_code) {
            case conversation_engine_error_success:
                cb_data->result.error_code = conversation_error_unknown; // 这里应该是success，但原枚举没有
                break;
            case conversation_engine_error_network:
                cb_data->result.error_code = conversation_error_network;
                break;
            case conversation_engine_error_auth:
                cb_data->result.error_code = conversation_error_auth;
                break;
            case conversation_engine_error_timeout:
                cb_data->result.error_code = conversation_error_timeout;
                break;
            case conversation_engine_error_audio_format:
                cb_data->result.error_code = conversation_error_audio_format;
                break;
            default:
                cb_data->result.error_code = conversation_error_unknown;
                break;
        }
    }
    
    message_t* message = calloc(1, sizeof(message_t));
    if (!message) {
        AI_INFO("Failed to allocate callback message");
        free(cb_data);
        return;
    }
    
    message->message_id = CONVERSATION_MESSAGE_CB;
    message->message_handler = conversation_message_cb_handler;
    message->message_data = cb_data;
    
    uv_async_queue_send(&ctx->user_asyncq, message);
}

/****************************************************************************
 * Message Handlers
 ****************************************************************************/

static int conversation_message_listener_handler(void* message_data)
{
    message_data_listener_t* data = (message_data_listener_t*)message_data;
    
    if (!data || !data->ctx) {
        return -EINVAL;
    }
    
    data->ctx->cb = data->cb;
    data->ctx->cookie = data->cookie;
    
    if (data->ctx->plugin && data->ctx->plugin->event_cb && data->ctx->engine) {
        return data->ctx->plugin->event_cb(data->ctx->engine, conversation_engine_event_cb, data->ctx);
    }
    
    return 0;
}

static int conversation_message_start_handler(void* message_data)
{
    message_data_start_t* data = (message_data_start_t*)message_data;
    
    if (!data || !data->ctx) {
        return -EINVAL;
    }
    
    if (data->ctx->plugin && data->ctx->plugin->start && data->ctx->engine) {
        conversation_engine_audio_info_t engine_audio_info = {
            .version = data->audio_info.version,
            .sample_rate = data->audio_info.sample_rate,
            .channels = data->audio_info.channels,
            .sample_bit = data->audio_info.sample_bit
        };
        
        if (data->audio_info.format) {
            strncpy(engine_audio_info.audio_type, data->audio_info.format, 
                    sizeof(engine_audio_info.audio_type) - 1);
        }
        
        return data->ctx->plugin->start(data->ctx->engine, &engine_audio_info);
    }
    
    return -ENOSYS;
}

static int conversation_message_send_audio_handler(void* message_data)
{
    message_data_send_audio_t* data = (message_data_send_audio_t*)message_data;
    
    if (!data || !data->ctx) {
        return -EINVAL;
    }
    
    if (data->ctx->plugin && data->ctx->plugin->send_audio && data->ctx->engine) {
        int ret = data->ctx->plugin->send_audio(data->ctx->engine, data->data, data->length);
        
        // 释放音频数据
        if (data->data) {
            free(data->data);
            data->data = NULL;
        }
        
        return ret;
    }
    
    return -ENOSYS;
}

static int conversation_message_finish_handler(void* message_data)
{
    message_data_finish_t* data = (message_data_finish_t*)message_data;
    
    if (!data || !data->ctx) {
        return -EINVAL;
    }
    
    if (data->ctx->plugin && data->ctx->plugin->finish && data->ctx->engine) {
        return data->ctx->plugin->finish(data->ctx->engine);
    }
    
    return -ENOSYS;
}

static int conversation_message_cancel_handler(void* message_data)
{
    message_data_cancel_t* data = (message_data_cancel_t*)message_data;
    
    if (!data || !data->ctx) {
        return -EINVAL;
    }
    
    if (data->ctx->plugin && data->ctx->plugin->cancel && data->ctx->engine) {
        return data->ctx->plugin->cancel(data->ctx->engine);
    }
    
    return -ENOSYS;
}

static int conversation_message_close_handler(void* message_data)
{
    message_data_close_t* data = (message_data_close_t*)message_data;
    
    if (!data || !data->ctx) {
        return -EINVAL;
    }
    
    data->ctx->is_closed = 1;
    data->ctx->state = CONVERSATION_STATE_CLOSED;
    
    if (data->ctx->plugin && data->ctx->engine) {
        conversation_plugin_uninit(data->ctx->plugin, data->ctx->engine, 1);
        data->ctx->engine = NULL;
    }
    
    return 0;
}

static int conversation_message_cb_handler(void* message_data)
{
    message_data_cb_t* data = (message_data_cb_t*)message_data;
    
    if (!data || !data->ctx || !data->ctx->cb) {
        return -EINVAL;
    }
    
    data->ctx->cb(data->event, &data->result, data->ctx->cookie);
    
    // 清理结果数据
    if (data->result.text) {
        free((void*)data->result.text);
    }
    if (data->result.audio_data) {
        free((void*)data->result.audio_data);
    }
    
    return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

conversation_handle_t ai_conversation_create_engine_with_auth(const conversation_init_params_t* param, 
                                                             const ai_auth_t* auth)
{
    conversation_context_t* ctx;
    conversation_engine_plugin_t* plugin;
    
    if (!param || !auth) {
        AI_INFO("Invalid parameters for conversation engine creation");
        return NULL;
    }
    
    // 验证参数
    if (param->timeout < CONVERSATION_MIN_TIMEOUT || param->timeout > CONVERSATION_MAX_TIMEOUT) {
        AI_INFO("Invalid timeout value: %d", param->timeout);
        return NULL;
    }
    
    // 获取插件
    plugin = conversation_get_plugin(param->engine_type);
    if (!plugin) {
        AI_INFO("Failed to get conversation plugin");
        return NULL;
    }
    
    // 创建上下文
    ctx = calloc(1, sizeof(conversation_context_t));
    if (!ctx) {
        AI_INFO("Failed to allocate conversation context");
        return NULL;
    }
    
    // 初始化异步队列
    ctx->loop = param->loop;
    ctx->user_loop = param->loop;
    
    ctx->asyncq = calloc(1, sizeof(uv_async_queue_t));
    if (!ctx->asyncq) {
        AI_INFO("Failed to allocate async queue");
        free(ctx);
        return NULL;
    }
    
    if (uv_async_queue_init(ctx->loop, ctx->asyncq, conversation_async_cb) < 0) {
        AI_INFO("Failed to initialize async queue");
        free(ctx->asyncq);
        free(ctx);
        return NULL;
    }
    
    if (uv_async_queue_init(ctx->user_loop, &ctx->user_asyncq, conversation_user_async_cb) < 0) {
        AI_INFO("Failed to initialize user async queue");
        uv_async_queue_uninit(ctx->asyncq);
        free(ctx->asyncq);
        free(ctx);
        return NULL;
    }
    
    // 设置引擎参数
    ctx->engine_param.loop = ctx->loop;
    ctx->engine_param.language = param->language;
    ctx->engine_param.voice = param->voice;
    ctx->engine_param.instructions = param->instructions;
    ctx->engine_param.timeout = param->timeout;
    ctx->engine_param.app_id = auth->app_id;
    ctx->engine_param.app_key = auth->app_key;
    ctx->engine_param.cb = conversation_async_cb;
    ctx->engine_param.opaque = ctx;
    
    // 初始化插件
    ctx->plugin = plugin;
    ctx->engine = conversation_plugin_init(plugin, &ctx->engine_param);
    if (!ctx->engine) {
        AI_INFO("Failed to initialize conversation plugin");
        uv_async_queue_uninit(&ctx->user_asyncq);
        uv_async_queue_uninit(ctx->asyncq);
        free(ctx->asyncq);
        free(ctx);
        return NULL;
    }
    
    ctx->state = CONVERSATION_STATE_INIT;
    
    AI_INFO("Conversation engine created successfully");
    return ctx;
}

int ai_conversation_set_listener(conversation_handle_t handle, 
                                conversation_callback_t callback, 
                                void* cookie)
{
    conversation_context_t* ctx = (conversation_context_t*)handle;
    
    if (!ctx || !callback) {
        return -EINVAL;
    }
    
    if (ctx->is_closed) {
        return -EBADF;
    }
    
    message_data_listener_t* data = calloc(1, sizeof(message_data_listener_t));
    if (!data) {
        return -ENOMEM;
    }
    
    data->ctx = ctx;
    data->cb = callback;
    data->cookie = cookie;
    
    message_t* message = calloc(1, sizeof(message_t));
    if (!message) {
        free(data);
        return -ENOMEM;
    }
    
    message->message_id = CONVERSATION_MESSAGE_LISTENER;
    message->message_handler = conversation_message_listener_handler;
    message->message_data = data;
    
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_conversation_start(conversation_handle_t handle, 
                         const conversation_audio_info_t* audio_info)
{
    conversation_context_t* ctx = (conversation_context_t*)handle;
    
    if (!ctx || !audio_info) {
        return -EINVAL;
    }
    
    if (ctx->is_closed) {
        return -EBADF;
    }
    
    message_data_start_t* data = calloc(1, sizeof(message_data_start_t));
    if (!data) {
        return -ENOMEM;
    }
    
    data->ctx = ctx;
    memcpy(&data->audio_info, audio_info, sizeof(conversation_audio_info_t));
    
    // 复制格式字符串
    if (audio_info->format) {
        data->audio_info.format = strdup(audio_info->format);
    }
    
    message_t* message = calloc(1, sizeof(message_t));
    if (!message) {
        if (data->audio_info.format) {
            free(data->audio_info.format);
        }
        free(data);
        return -ENOMEM;
    }
    
    message->message_id = CONVERSATION_MESSAGE_START;
    message->message_handler = conversation_message_start_handler;
    message->message_data = data;
    
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_conversation_send_audio(conversation_handle_t handle, 
                              const void* data, 
                              int length)
{
    conversation_context_t* ctx = (conversation_context_t*)handle;
    
    if (!ctx || !data || length <= 0) {
        return -EINVAL;
    }
    
    if (ctx->is_closed) {
        return -EBADF;
    }
    
    message_data_send_audio_t* msg_data = calloc(1, sizeof(message_data_send_audio_t));
    if (!msg_data) {
        return -ENOMEM;
    }
    
    // 复制音频数据
    msg_data->data = malloc(length);
    if (!msg_data->data) {
        free(msg_data);
        return -ENOMEM;
    }
    memcpy(msg_data->data, data, length);
    
    msg_data->ctx = ctx;
    msg_data->length = length;
    
    message_t* message = calloc(1, sizeof(message_t));
    if (!message) {
        free(msg_data->data);
        free(msg_data);
        return -ENOMEM;
    }
    
    message->message_id = CONVERSATION_MESSAGE_SEND_AUDIO;
    message->message_handler = conversation_message_send_audio_handler;
    message->message_data = msg_data;
    
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_conversation_finish(conversation_handle_t handle)
{
    conversation_context_t* ctx = (conversation_context_t*)handle;
    
    if (!ctx) {
        return -EINVAL;
    }
    
    if (ctx->is_closed) {
        return -EBADF;
    }
    
    message_data_finish_t* data = calloc(1, sizeof(message_data_finish_t));
    if (!data) {
        return -ENOMEM;
    }
    
    data->ctx = ctx;
    
    message_t* message = calloc(1, sizeof(message_t));
    if (!message) {
        free(data);
        return -ENOMEM;
    }
    
    message->message_id = CONVERSATION_MESSAGE_FINISH;
    message->message_handler = conversation_message_finish_handler;
    message->message_data = data;
    
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_conversation_cancel(conversation_handle_t handle)
{
    conversation_context_t* ctx = (conversation_context_t*)handle;
    
    if (!ctx) {
        return -EINVAL;
    }
    
    if (ctx->is_closed) {
        return -EBADF;
    }
    
    message_data_cancel_t* data = calloc(1, sizeof(message_data_cancel_t));
    if (!data) {
        return -ENOMEM;
    }
    
    data->ctx = ctx;
    
    message_t* message = calloc(1, sizeof(message_t));
    if (!message) {
        free(data);
        return -ENOMEM;
    }
    
    message->message_id = CONVERSATION_MESSAGE_CANCEL;
    message->message_handler = conversation_message_cancel_handler;
    message->message_data = data;
    
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_conversation_is_busy(conversation_handle_t handle)
{
    conversation_context_t* ctx = (conversation_context_t*)handle;
    
    if (!ctx) {
        return -EINVAL;
    }
    
    if (ctx->is_closed) {
        return -EBADF;
    }
    
    return (ctx->state == CONVERSATION_STATE_ACTIVE) ? 1 : 0;
}

int ai_conversation_close(conversation_handle_t handle)
{
    conversation_context_t* ctx = (conversation_context_t*)handle;
    
    if (!ctx) {
        return -EINVAL;
    }
    
    if (ctx->is_closed) {
        return 0;
    }
    
    message_data_close_t* data = calloc(1, sizeof(message_data_close_t));
    if (!data) {
        return -ENOMEM;
    }
    
    data->ctx = ctx;
    
    message_t* message = calloc(1, sizeof(message_t));
    if (!message) {
        free(data);
        return -ENOMEM;
    }
    
    message->message_id = CONVERSATION_MESSAGE_CLOSE;
    message->message_handler = conversation_message_close_handler;
    message->message_data = data;
    
    int ret = uv_async_queue_send(ctx->asyncq, message);
    
    // 等待关闭完成
    while (!ctx->is_closed && uv_loop_alive(ctx->loop)) {
        uv_run(ctx->loop, UV_RUN_ONCE);
    }
    
    // 清理资源
    uv_async_queue_uninit(&ctx->user_asyncq);
    uv_async_queue_uninit(ctx->asyncq);
    free(ctx->asyncq);
    free(ctx);
    
    AI_INFO("Conversation engine closed");
    return ret;
} 