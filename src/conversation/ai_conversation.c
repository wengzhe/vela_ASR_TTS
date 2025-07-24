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
#include <media_api.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <uv.h>
#include <uv_async_queue.h>

#include "ai_conversation.h"
#include "ai_common.h"
#include "ai_conversation_plugin.h"
#include "ai_ring_buffer.h"

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
    void* recorder_handle; // recorder handle
    void* player_handle;   // player handle
    void* focus_handle;
    uv_loop_t* loop;
    uv_loop_t* user_loop;
    uv_async_queue_t* asyncq;
    uv_async_queue_t user_asyncq;
    uv_pipe_t* recorder_pipe;
    uv_pipe_t* player_pipe;
    char* format;
    conversation_callback_t cb;
    void* cookie;
    conversation_state_t state;
    int is_closed;
    int is_send_finished;
    conversation_engine_init_params_t engine_param;
    ai_ring_buffer_t audio_buffer;
    char* audio_frame_buf;
    uv_write_t write_req;
    int data_end;
} conversation_context_t;

typedef enum {
    CONVERSATION_MESSAGE_CREATE_ENGINE,
    CONVERSATION_MESSAGE_LISTENER,
    CONVERSATION_MESSAGE_START,
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
static int conversation_message_finish_handler(void* message_data);
static int conversation_message_cancel_handler(void* message_data);
static int conversation_message_close_handler(void* message_data);
static int conversation_message_cb_handler(void* message_data);

// Media callbacks
static void alloc_read_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
static void read_buffer_cb(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf);
static void media_recorder_prepare_connect_cb(void* cookie, int ret, void* obj);
static void media_recorder_open_cb(void* cookie, int ret);
static void media_recorder_start_cb(void* cookie, int ret);
static void media_recorder_close_cb(void* cookie, int ret);
static void media_recorder_event_callback(void* cookie, int event, int ret, const char* extra);

static void media_player_prepare_connect_cb(void* cookie, int ret, void* obj);
static void media_player_open_cb(void* cookie, int ret);
static void media_player_start_cb(void* cookie, int ret);
static void media_player_close_cb(void* cookie, int ret);
static void media_player_event_callback(void* cookie, int event, int ret, const char* extra);
static void write_audio_data_cb(uv_write_t* req, int status);

static int ai_conversation_init_recorder(conversation_context_t* ctx);
static int ai_conversation_init_player(conversation_context_t* ctx);
static int ai_conversation_play_audio(conversation_context_t* ctx, const void* data, int length);

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
            // 播放接收到的音频数据
            if (result && result->audio_data && result->audio_length > 0) {
                ai_conversation_play_audio(ctx, result->audio_data, result->audio_length);
            }
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
    int ret;
    
    if (!data || !data->ctx) {
        return -EINVAL;
    }
    
    conversation_context_t* ctx = data->ctx;
    
    // 设置音频格式
    if (data->audio_info.format) {
        if (ctx->format) {
            free(ctx->format);
        }
        ctx->format = strdup(data->audio_info.format);
    }
    
    // 初始化recorder
    ret = ai_conversation_init_recorder(ctx);
    if (ret < 0) {
        AI_INFO("Failed to initialize recorder");
        return ret;
    }
    
    // 初始化player
    ret = ai_conversation_init_player(ctx);
    if (ret < 0) {
        AI_INFO("Failed to initialize player");
        return ret;
    }
    
    // 启动插件引擎
    if (ctx->plugin && ctx->plugin->start && ctx->engine) {
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
        
        ret = ctx->plugin->start(ctx->engine, &engine_audio_info);
        if (ret < 0) {
            AI_INFO("Failed to start conversation engine");
            return ret;
        }
    }
    
    // 启动recorder
    ret = media_uv_recorder_start(ctx->recorder_handle, media_recorder_start_cb, ctx);
    if (ret < 0) {
        AI_INFO("Failed to start recorder");
        return ret;
    }
    
    // 启动player
    ret = media_uv_player_start(ctx->player_handle, media_player_start_cb, ctx);
    if (ret < 0) {
        AI_INFO("Failed to start player");
        return ret;
    }
    
    ctx->state = CONVERSATION_STATE_ACTIVE;
    
    return 0;
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
    int ret = 0;
    
    if (!data || !data->ctx) {
        return -EINVAL;
    }
    
    conversation_context_t* ctx = data->ctx;
    
    if (ctx->state == CONVERSATION_STATE_CLOSED) {
        return 0;
    }
    
    ctx->is_closed = 1;
    ctx->state = CONVERSATION_STATE_CLOSED;
    
    // 清理格式字符串
    if (ctx->format) {
        free(ctx->format);
        ctx->format = NULL;
    }
    
    // 清理插件引擎
    if (ctx->plugin && ctx->engine) {
        conversation_plugin_uninit(ctx->plugin, ctx->engine, 1);
        ctx->engine = NULL;
    }
    
    // 关闭recorder
    if (ctx->recorder_handle) {
        ret = media_uv_recorder_close(ctx->recorder_handle, media_recorder_close_cb);
        ctx->recorder_handle = NULL;
    }
    
    // 关闭player
    if (ctx->player_handle) {
        ret = media_uv_player_close(ctx->player_handle, 0, media_player_close_cb);
        ctx->player_handle = NULL;
    }
    
    // 清理focus
    if (ctx->focus_handle) {
        media_focus_abandon(ctx->focus_handle);
        ctx->focus_handle = NULL;
    }
    
    // 清理音频缓冲区
    if (ctx->audio_buffer.buffer) {
        free(ctx->audio_buffer.buffer);
        ctx->audio_buffer.buffer = NULL;
    }
    
    if (ctx->audio_frame_buf) {
        free(ctx->audio_frame_buf);
        ctx->audio_frame_buf = NULL;
    }
    
    AI_INFO("ai_conversation_close_handler");
    
    return ret;
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
 * Media Callback Functions
 ****************************************************************************/

static void alloc_read_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf)
{
    buf->base = (char*)calloc(1, suggested_size);
    buf->len = suggested_size;
}

static void read_buffer_cb(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf)
{
    conversation_context_t* ctx = uv_handle_get_data((uv_handle_t*)client);
    
    if (ctx && ctx->plugin && ctx->plugin->write_audio && ctx->engine && nread > 0) {
        ctx->plugin->write_audio(ctx->engine, buf->base, nread);
        static int count = 0;
        if (count % 20 == 0)
            AI_INFO("conversation recorder read audio data: %d\n", nread);
        count++;
    }
    
    if (buf->base) {
        free(buf->base);
    }
}

static void media_recorder_prepare_connect_cb(void* cookie, int ret, void* obj)
{
    conversation_context_t* ctx = cookie;

    if (ret < 0) {
        AI_INFO("conversation recorder prepare connect cb error:%d\n", ret);
        return;
    }

    ctx->recorder_pipe = (uv_pipe_t*)obj;
    uv_handle_set_data((uv_handle_t*)ctx->recorder_pipe, ctx);
    uv_read_start((uv_stream_t*)ctx->recorder_pipe, alloc_read_buffer, read_buffer_cb);
}

static void media_recorder_open_cb(void* cookie, int ret)
{
    conversation_context_t* ctx = cookie;

    if (ret < 0) {
        AI_INFO("conversation recorder open cb error:%d", ret);
    }
    AI_INFO("conversation recorder open cb:%d", ret);
}

static void media_recorder_start_cb(void* cookie, int ret)
{
    conversation_context_t* ctx = cookie;

    if (ret < 0) {
        AI_INFO("conversation recorder start cb error:%d", ret);
    }
    AI_INFO("conversation recorder start cb:%d", ret);
}

static void media_recorder_close_cb(void* cookie, int ret)
{
    AI_INFO("conversation recorder close cb:%d", ret);
}

static void media_recorder_event_callback(void* cookie, int event, int ret, const char* extra)
{
    conversation_context_t* ctx = cookie;

    if (ret < 0) {
        AI_INFO("conversation recorder event error:%d", ret);
    }

    switch (event) {
    case MEDIA_EVENT_NOP:
        break;
    case MEDIA_EVENT_PREPARED:
        break;
    case MEDIA_EVENT_STARTED:
        break;
    case MEDIA_EVENT_PAUSED:
        break;
    case MEDIA_EVENT_STOPPED:
        break;
    case MEDIA_EVENT_COMPLETED:
        break;
    case MEDIA_EVENT_SEEKED:
        break;
    default:
        return;
    }

    AI_INFO("conversation recorder event callback event:%d ret:%d", event, ret);
}

static void media_player_prepare_connect_cb(void* cookie, int ret, void* obj)
{
    conversation_context_t* ctx = cookie;

    if (ret < 0) {
        AI_INFO("conversation player prepare connect cb error:%d\n", ret);
        return;
    }

    ctx->player_pipe = (uv_pipe_t*)obj;
    uv_handle_set_data((uv_handle_t*)ctx->player_pipe, ctx);
}

static void media_player_open_cb(void* cookie, int ret)
{
    conversation_context_t* ctx = cookie;

    if (ret < 0) {
        AI_INFO("conversation player open cb error:%d", ret);
    }
    AI_INFO("conversation player open cb:%d", ret);
}

static void media_player_start_cb(void* cookie, int ret)
{
    conversation_context_t* ctx = cookie;

    if (ret < 0) {
        AI_INFO("conversation player start cb error:%d", ret);
    }
    AI_INFO("conversation player start cb:%d", ret);
}

static void media_player_close_cb(void* cookie, int ret)
{
    AI_INFO("conversation player close cb:%d", ret);
}

static void media_player_event_callback(void* cookie, int event, int ret, const char* extra)
{
    conversation_context_t* ctx = cookie;

    if (ret < 0) {
        AI_INFO("conversation player event error:%d", ret);
    }

    switch (event) {
    case MEDIA_EVENT_NOP:
        break;
    case MEDIA_EVENT_PREPARED:
        break;
    case MEDIA_EVENT_STARTED:
        break;
    case MEDIA_EVENT_PAUSED:
        break;
    case MEDIA_EVENT_STOPPED:
        break;
    case MEDIA_EVENT_COMPLETED:
        break;
    case MEDIA_EVENT_SEEKED:
        break;
    default:
        return;
    }

    AI_INFO("conversation player event callback event:%d ret:%d", event, ret);
}

static void write_audio_data_cb(uv_write_t* req, int status)
{
    conversation_context_t* ctx = (conversation_context_t*)req->data;
    
    if (status < 0) {
        AI_INFO("write audio data error:%d", status);
        return;
    }
    
    // 继续写入缓冲区中的数据
    if (ai_ring_buffer_num_items(&ctx->audio_buffer) > 0) {
        size_t available = ai_ring_buffer_num_items(&ctx->audio_buffer);
        size_t to_write = available > 4096 ? 4096 : available;
        
        ai_ring_buffer_dequeue_arr(&ctx->audio_buffer, ctx->audio_frame_buf, to_write);
        
        uv_buf_t buf = uv_buf_init(ctx->audio_frame_buf, to_write);
        ctx->write_req.data = ctx;
        uv_write(&ctx->write_req, (uv_stream_t*)ctx->player_pipe, &buf, 1, write_audio_data_cb);
    }
}

static int ai_conversation_init_recorder(conversation_context_t* ctx)
{
    void* handle;
    int ret;
    char* format;
    char* stream;

    if (!ctx) {
        return -EINVAL;
    }

    format = ctx->format ? ctx->format : "pcm";
    stream = "default";

    handle = media_uv_recorder_open(ctx->loop, stream, media_recorder_open_cb, ctx);
    if (!handle) {
        AI_INFO("conversation recorder open failed");
        return -1;
    }

    ret = media_uv_recorder_listen(handle, media_recorder_event_callback);
    if (ret < 0) {
        AI_INFO("conversation recorder listen failed");
        media_uv_recorder_close(handle, media_recorder_close_cb);
        return ret;
    }

    ret = media_uv_recorder_prepare(handle, NULL, format,
        media_recorder_prepare_connect_cb, NULL, NULL);
    if (ret < 0) {
        AI_INFO("conversation recorder prepare failed");
        media_uv_recorder_close(handle, media_recorder_close_cb);
        return ret;
    }

    ctx->recorder_handle = handle;
    AI_INFO("ai_conversation_init_recorder %p\n", ctx->recorder_handle);

    return 0;
}

static int ai_conversation_init_player(conversation_context_t* ctx)
{
    void* handle;
    int ret;
    char* format;
    char* stream;

    if (!ctx) {
        return -EINVAL;
    }

    format = ctx->format ? ctx->format : "pcm";
    stream = "default";

    handle = media_uv_player_open(ctx->loop, stream, media_player_open_cb, ctx);
    if (!handle) {
        AI_INFO("conversation player open failed");
        return -1;
    }

    ret = media_uv_player_listen(handle, media_player_event_callback);
    if (ret < 0) {
        AI_INFO("conversation player listen failed");
        media_uv_player_close(handle, 0, media_player_close_cb);
        return ret;
    }

    ret = media_uv_player_prepare(handle, NULL, format,
        media_player_prepare_connect_cb, NULL, NULL);
    if (ret < 0) {
        AI_INFO("conversation player prepare failed");
        media_uv_player_close(handle, 0, media_player_close_cb);
        return ret;
    }

    ctx->player_handle = handle;
    
    // 初始化音频缓冲区
    ctx->audio_frame_buf = malloc(4096);
    if (!ctx->audio_frame_buf) {
        AI_INFO("Failed to allocate audio frame buffer");
        media_uv_player_close(handle, 0, media_player_close_cb);
        return -ENOMEM;
    }
    
    char* audio_buffer_data = malloc(64 * 1024);
    if (!audio_buffer_data) {
        AI_INFO("Failed to allocate audio buffer");
        free(ctx->audio_frame_buf);
        media_uv_player_close(handle, 0, media_player_close_cb);
        return -ENOMEM;
    }
    
    ai_ring_buffer_init(&ctx->audio_buffer, audio_buffer_data, 64 * 1024);
    
    AI_INFO("ai_conversation_init_player %p\n", ctx->player_handle);

    return 0;
}

static int ai_conversation_play_audio(conversation_context_t* ctx, const void* data, int length)
{
    if (!ctx || !data || length <= 0 || !ctx->player_pipe) {
        return -EINVAL;
    }
    
    // 将音频数据加入缓冲区
    if (ai_ring_buffer_space_avail(&ctx->audio_buffer) < length) {
        AI_INFO("Audio buffer full, dropping data");
        return -ENOSPC;
    }
    
    ai_ring_buffer_enqueue_arr(&ctx->audio_buffer, (const char*)data, length);
    
    // 如果当前没有写操作在进行，启动写操作
    if (ai_ring_buffer_num_items(&ctx->audio_buffer) > 0 && !ctx->write_req.data) {
        size_t available = ai_ring_buffer_num_items(&ctx->audio_buffer);
        size_t to_write = available > 4096 ? 4096 : available;
        
        ai_ring_buffer_dequeue_arr(&ctx->audio_buffer, ctx->audio_frame_buf, to_write);
        
        uv_buf_t buf = uv_buf_init(ctx->audio_frame_buf, to_write);
        ctx->write_req.data = ctx;
        return uv_write(&ctx->write_req, (uv_stream_t*)ctx->player_pipe, &buf, 1, write_audio_data_cb);
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