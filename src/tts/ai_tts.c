/****************************************************************************
 * frameworks/ai/src/tts/ai_tts.c
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
#include <time.h>
#include <uv.h>
#include <uv_async_queue.h>

#include "ai_common.h"
#include "ai_ring_buffer.h"
#include "ai_tts.h"
#include "ai_tts_plugin.h"

#define TTS_DEFAULT_SILENCE_TIMEOUT 3000
#define TTS_MAX_SILENCE_TIMEOUT 15000
#define TTS_BUFFER_MAX_SIZE 128 * 1024

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef enum {
    TTS_STATE_INIT,
    TTS_STATE_START,
    TTS_STATE_FINISH,
    TTS_STATE_CLOSE
} tts_state_t;

typedef struct tts_context {
    tts_engine_plugin_t* plugin;
    void* engine;
    void* handle; // player handle
    void* focus_handle;
    uv_loop_t* loop;
    uv_loop_t* user_loop;
    uv_async_queue_t* asyncq;
    uv_async_queue_t user_asyncq;
    uv_pipe_t* pipe;
    char* format;
    tts_callback_t cb;
    void* cookie;
    tts_state_t state;
    int is_send_finished;
    tts_engine_init_params_t voice_param;
    uv_write_t write_req;
    ai_ring_buffer_t buffer;
    char* frame_buf;
    int data_end;
} tts_context_t;

typedef enum {
    TTS_MESSAGE_CREATE_ENGINE,
    TTS_MESSAGE_LISTENER,
    TTS_MESSAGE_START,
    TTS_MESSAGE_FINISH,
    TTS_MESSAGE_IS_BUSY,
    TTS_MESSAGE_CLOSE,
    TTS_MESSAGE_CB
} message_id_t;

typedef int (*message_handler_t)(void* message_data);

typedef struct message_s {
    message_id_t message_id;
    message_handler_t message_handler;
    void* message_data;
} message_t;

typedef struct message_data_listener_s {
    tts_context_t* ctx;
    tts_callback_t cb;
    void* cookie;
} message_data_listener_t;

typedef struct message_data_speak_s {
    tts_context_t* ctx;
    tts_audio_info_t audio_info;
    char* text;
} message_data_speak_t;

typedef struct message_data_finish_s {
    tts_context_t* ctx;
} message_data_finish_t;

typedef struct message_data_is_busy_s {
    tts_context_t* ctx;
} message_data_is_busy_t;

typedef struct message_data_close_s {
    tts_context_t* ctx;
} message_data_close_t;

typedef struct message_data_cb_s {
    tts_context_t* ctx;
    tts_engine_event_t event;
    tts_result_t* result;
} message_data_cb_t;

extern tts_engine_plugin_t volc_tts_engine_plugin;
static void ai_tts_voice_callback(tts_engine_event_t event, const tts_engine_result_t* result, void* cookie);
static void ai_tts_write_buf(tts_context_t* ctx);
static int ai_tts_finish_handler(tts_context_t* ctx, int pending);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void media_player_write_cb(uv_write_t* req, int status)
{
    tts_context_t* ctx = uv_req_get_data((uv_req_t*)req);
    int len;

    if (status < 0)
        AI_INFO("tts player write cb error:%d", status);

    len = ai_ring_buffer_num_items(&ctx->buffer);
    if (ctx->data_end && len == 0) {
        ai_tts_finish_handler(ctx, 1);
        ai_tts_voice_callback(tts_engine_event_complete, NULL, ctx);
    }

    free(ctx->frame_buf);
    ctx->frame_buf = NULL;
    ai_tts_write_buf(ctx);
}

static void ai_tts_write_buf(tts_context_t* ctx)
{
    char* frame_buffer;
    uv_buf_t iov;
    int len;

    if (!ctx || !ctx->pipe || !ctx->buffer.buffer)
        return;

    len = ai_ring_buffer_num_items(&ctx->buffer);
    if (len <= 0 || ctx->frame_buf) {
        return;
    }

    frame_buffer = (char*)malloc(len);
    ai_ring_buffer_dequeue_arr(&ctx->buffer, frame_buffer, len);
    iov = uv_buf_init(frame_buffer, len);
    ctx->frame_buf = frame_buffer;
    uv_req_set_data((uv_req_t*)&ctx->write_req, ctx);
    uv_write((uv_write_t*)&ctx->write_req, (uv_stream_t*)ctx->pipe, &iov, 1, media_player_write_cb);
}

static void ai_tts_send_error(tts_context_t* ctx, tts_error_t error)
{
    tts_engine_result_t result;

    result.len = 0;
    result.result = NULL;
    result.error_code = error;
    ai_tts_voice_callback(tts_engine_event_error, &result, ctx);
}

static void media_player_prepare_connect_cb(void* cookie, int ret, void* obj)
{
    tts_context_t* ctx = cookie;

    if (ret < 0) {
        ai_tts_send_error(ctx, tts_error_media);
        AI_INFO("tts player prepare connect cb error:%d\n", ret);
        return;
    }

    ctx->pipe = (uv_pipe_t*)obj;
    ai_tts_write_buf(ctx);
    AI_INFO("tts player prepare connect cb:%d cookie:%p pipe:%p\n", ret, cookie, obj);
}

static void media_player_open_cb(void* cookie, int ret)
{
    tts_context_t* ctx = cookie;

    if (ret < 0)
        ai_tts_send_error(ctx, tts_error_media);
    AI_INFO("tts player open cb:%d", ret);
}

static void media_player_start_cb(void* cookie, int ret)
{
    tts_context_t* ctx = cookie;

    if (ret < 0)
        ai_tts_send_error(ctx, tts_error_media);
    AI_INFO("tts player start cb:%d", ret);
}

static void media_player_close_cb(void* cookie, int ret)
{
    AI_INFO("tts player close cb:%d", ret);
}

static void media_player_event_callback(void* cookie, int event, int ret,
    const char* extra)
{
    tts_context_t* ctx = cookie;

    if (ret < 0)
        ai_tts_send_error(ctx, tts_error_media);

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

    AI_INFO("tts player event callback event:%d ret:%d", event, ret);
}

static int ai_tts_close_handler(tts_context_t* ctx)
{
    int ret = 0;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->state == TTS_STATE_CLOSE)
        return 0;
    ctx->state = TTS_STATE_CLOSE;

    if (ctx->format) {
        free(ctx->format);
        ctx->format = NULL;
    }

    if (ctx->engine) {
        tts_plugin_uninit(ctx->plugin, ctx->engine, 0);
        ctx->engine = NULL;
    }

    if (ctx->handle) {
        ret = media_uv_player_close(ctx->handle, 0, media_player_close_cb);
        ctx->handle = NULL;
    }

    if (ctx->focus_handle) {
        media_focus_abandon(ctx->focus_handle);
        ctx->focus_handle = NULL;
    }

    if (ctx->buffer.buffer) {
        free(ctx->buffer.buffer);
        ctx->buffer.buffer = NULL;
    }

    free(ctx);
    ctx = NULL;

    AI_INFO("ai_tts_close_handler");

    return ret;
}

static int ai_tts_finish_handler(tts_context_t* ctx, int pending)
{
    int ret = 0;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->state == TTS_STATE_FINISH)
        return 0;

    if (ctx->handle != NULL) {
        ret = media_uv_player_close(ctx->handle, pending, media_player_close_cb);
        if (ret < 0)
            AI_INFO("close player failed:%d", ret);
        ctx->handle = NULL;
        AI_INFO("ai_tts close media!\n");
    }

    if (ctx->focus_handle) {
        media_focus_abandon(ctx->focus_handle);
        ctx->focus_handle = NULL;
        AI_INFO("ai_tts abandon focus!\n");
    }

    if (ctx->engine != NULL) {
        ret = ctx->plugin->stop(ctx->engine);
        AI_INFO("ai_tts stop tts!\n");
    }

    if (ctx->buffer.buffer) {
        free(ctx->buffer.buffer);
        ctx->buffer.buffer = NULL;
    }

    ctx->state = TTS_STATE_FINISH;
    AI_INFO("ai_tts_finish_handler");

    return ret;
}

static void ai_tts_focus_callback(int suggestion, void* cookie)
{
    tts_context_t* ctx = cookie;

    if (suggestion != MEDIA_FOCUS_PLAY) {
        ai_tts_finish_handler(ctx, 0);
        ai_tts_voice_callback(tts_engine_event_complete, NULL, ctx);
    }

    AI_INFO("tts player focus suggestion:%d", suggestion);
}

static int ai_tts_init_player(tts_context_t* ctx)
{
    const char* format = ctx->format;
    int init_suggestion;
    char* stream = "Music";
    void* handle = NULL;

    ctx->focus_handle = media_focus_request(&init_suggestion, MEDIA_SCENARIO_TTS, ai_tts_focus_callback, ctx);
    if (init_suggestion != MEDIA_FOCUS_PLAY && ctx->focus_handle) {
        AI_INFO("tts player focus failed");
        media_focus_abandon(ctx->focus_handle);
        ctx->focus_handle = NULL;
        goto failed;
    }

    handle = media_uv_player_open(ctx->loop, stream, media_player_open_cb, ctx);
    if (handle == NULL) {
        AI_INFO("tts player open failed");
        goto failed;
    }

    int ret = media_uv_player_listen(handle, media_player_event_callback);
    if (ret < 0) {
        AI_INFO("tts player listen failed");
        media_uv_player_close(handle, 0, media_player_close_cb);
        goto failed;
    }

    ret = media_uv_player_prepare(handle, NULL, format,
        media_player_prepare_connect_cb, NULL, NULL);
    if (ret < 0) {
        AI_INFO("tts player prepare failed");
        media_uv_player_close(handle, 0, media_player_close_cb);
        goto failed;
    }

    ctx->handle = handle;
    AI_INFO("ai_tts_init_player %p\n", ctx->handle);

    return 0;
failed:
    return -EPERM;
}

static int ai_tts_callback_l(void* message_data)
{
    message_data_cb_t* data = (message_data_cb_t*)message_data;
    tts_context_t* ctx = data->ctx;
    tts_event_t event = data->event;
    tts_result_t* tts_result = data->result;

    if (ctx->cb)
        ctx->cb(event, tts_result, ctx->cookie);

    if (tts_result) {
        free(tts_result->result);
        free(tts_result);
    }

    return 0;
}

static void ai_tts_voice_callback(tts_engine_event_t event, const tts_engine_result_t* result, void* cookie)
{
    tts_context_t* ctx = cookie;
    tts_result_t* tts_result = NULL;
    int buf_num;

    if (ctx->cb == NULL)
        return;

    if (ctx->is_send_finished || ctx->state == TTS_STATE_CLOSE)
        return;

    if (result)
        AI_INFO("ai_tts result info event:%d len:%d error:%d\n", event, result->len, result->error_code);

    if (result) {
        tts_result = (tts_result_t*)malloc(sizeof(tts_result_t));
        if (result->result != NULL && result->len > 0) {
            tts_result->result = (char*)malloc(result->len);
            strlcpy(tts_result->result, result->result, result->len);

            if (ctx->buffer.buffer == NULL) {
                AI_INFO("asr_tts init ring buffer\n");
                char* buffer = (char*)malloc(TTS_BUFFER_MAX_SIZE);
                ai_ring_buffer_init(&ctx->buffer, buffer, TTS_BUFFER_MAX_SIZE);
            }

            if (ai_ring_buffer_is_full(&ctx->buffer)) {
                AI_INFO("asr_volc ring buffer is full\n");
                ai_ring_buffer_clear_arr(&ctx->buffer, result->len);
            }

            buf_num = ai_ring_buffer_num_items(&ctx->buffer);
            if (buf_num == 0) {
                ai_ring_buffer_queue_arr(&ctx->buffer, result->result, result->len);
                ai_tts_write_buf(ctx);
            } else
                ai_ring_buffer_queue_arr(&ctx->buffer, result->result, result->len);
        } else if (tts_engine_event_result == event && result->len == 0) {
            ai_tts_write_buf(ctx);
            char zero_buf[32000] = { 0 };
            ai_ring_buffer_queue_arr(&ctx->buffer, zero_buf, 32000);
            ai_tts_write_buf(ctx);
            ctx->data_end = 1;
            free(tts_result);
            AI_INFO("ai_tts_voice_callback data end");
            return;
        } else
            tts_result->result = NULL;
        tts_result->len = result->len;
        if (result->error_code != 0)
            tts_result->error_code = tts_error_failed;
        else
            tts_result->error_code = 0;
        AI_INFO("ai_tts_voice_callback:%d\n", result->len);
    }

    if (tts_engine_event_complete == event || tts_engine_event_error == event) {
        ai_tts_finish_handler(ctx, 0);
        AI_INFO("ai_tts_voice_callback complete or error");
        ctx->is_send_finished = true;
    }

    message_data_cb_t* cb = (message_data_cb_t*)malloc(sizeof(message_data_cb_t));
    cb->ctx = ctx;
    cb->event = event;
    cb->result = tts_result;
    if (ctx->user_loop) {
        message_t* message = (message_t*)malloc(sizeof(message_t));
        message->message_id = TTS_MESSAGE_CB;
        message->message_handler = ai_tts_callback_l;
        message->message_data = cb;
        uv_async_queue_send(&(ctx->user_asyncq), message);
    } else {
        ai_tts_callback_l(cb);
        free(cb);
    }
}

static void ai_tts_async_cb(uv_async_queue_t* handle, void* data)
{
    AI_INFO("ai_tts_async_cb");

    message_t* message = (message_t*)data;

    if (message->message_handler)
        message->message_handler(message->message_data);

    free(message->message_data);
    free(message);
}

static void ai_tts_map_params(tts_context_t* ctx, const tts_init_params_t* in_param, tts_engine_init_params_t* out_param)
{
    out_param->loop = in_param->loop;
    out_param->language = in_param->language ?: "zh-CN";
    if (in_param->silence_timeout <= TTS_MAX_SILENCE_TIMEOUT && in_param->silence_timeout > 0)
        out_param->silence_timeout = in_param->silence_timeout;
    else if (in_param->silence_timeout > TTS_MAX_SILENCE_TIMEOUT)
        out_param->silence_timeout = TTS_MAX_SILENCE_TIMEOUT;
    else
        out_param->silence_timeout = TTS_DEFAULT_SILENCE_TIMEOUT;
    out_param->app_id = in_param->app_id ?: "";
    out_param->app_key = in_param->app_key ?: "";
    out_param->cb = ai_tts_async_cb;
    out_param->opaque = ctx;
}

static int ai_tts_set_listener_l(void* message_data)
{
    message_data_listener_t* data = (message_data_listener_t*)message_data;
    tts_context_t* ctx = data->ctx;
    tts_callback_t callback = data->cb;
    void* cookie = data->cookie;

    if (ctx == NULL || ctx->engine == NULL)
        return -1;

    ctx->cb = callback;
    ctx->cookie = cookie;

    AI_INFO("ai_tts_set_listener_l");

    return ctx->plugin->event_cb(ctx->engine, ai_tts_voice_callback, ctx);
}

static int ai_tts_create_format(tts_context_t* ctx, const char* format)
{
    int len;

    if (!format)
        return -EINVAL;

    len = strlen(format) + 1;
    char* temp = (char*)realloc(ctx->format, len);
    if (temp == NULL) {
        return -ENOMEM;
    }
    ctx->format = temp;
    strlcpy(ctx->format, format, len);

    return 0;
}

static int ai_tts_speak_l(void* message_data)
{
    message_data_speak_t* data = (message_data_speak_t*)message_data;
    tts_context_t* ctx = data->ctx;
    const tts_audio_info_t* audio_info = &data->audio_info;
    tts_engine_env_params_t* env;
    int ret = 0;

    AI_INFO("ai_tts_speak_l before\n");

    if (ctx == NULL || ctx->engine == NULL)
        return -EINVAL;

    if (ctx->state == TTS_STATE_START)
        return 0;
    ctx->state = TTS_STATE_START;

    env = ctx->plugin->get_env(ctx->engine);
    if (audio_info && audio_info->format && !env->force_format) {
        ret = ai_tts_create_format(ctx, audio_info->format);
        free(audio_info->format);
    } else
        ret = ai_tts_create_format(ctx, env->format);
    if (ret < 0)
        goto failed;

    ctx->is_send_finished = false;
    ctx->data_end = 0;

    ret = ctx->plugin->speak(ctx->engine, data->text, NULL);
    if (data->text)
        free(data->text);
    if (ret < 0)
        goto failed;

    if (ctx->handle != NULL) {
        AI_INFO("tts player already opened");
        return 0;
    }

    ret = ai_tts_init_player(ctx);
    if (ret < 0)
        goto failed;

    ret = media_uv_player_start(ctx->handle, media_player_start_cb, ctx);
    if (ret < 0)
        goto failed;

    ai_tts_voice_callback(tts_engine_event_start, NULL, ctx);

    AI_INFO("ai_tts_speak_l");

    return ret;
failed:
    AI_INFO("ai_tts_speak_l failed");
    media_uv_player_close(ctx->handle, 0, media_player_close_cb);
    ctx->handle = NULL;
    return ret;
}

static int ai_tts_stop_l(void* message_data)
{
    int ret;

    AI_INFO("ai_tts_stop_l");
    message_data_finish_t* data = (message_data_finish_t*)message_data;
    ret = ai_tts_finish_handler(data->ctx, 0);
    ai_tts_voice_callback(tts_engine_event_complete, NULL, data->ctx);
    return ret;
}

static int ai_tts_close_l(void* message_data)
{
    AI_INFO("ai_tts_close_l");
    message_data_close_t* data = (message_data_close_t*)message_data;
    return ai_tts_close_handler(data->ctx);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

tts_handle_t ai_tts_create_engine(const tts_init_params_t* param)
{
    tts_context_t* ctx;
    tts_engine_plugin_t* plugin;
    tts_engine_env_params_t* env;
    int ret;

    if (param == NULL)
        return NULL;

    AI_INFO("ai_tts_create_engine type: %d", param->engine_type);

    if (param->engine_type == tts_engine_type_volc)
        plugin = &volc_tts_engine_plugin;
    else {
        AI_INFO("unknown engine type: %d", param->engine_type);
        return NULL;
    }

    ctx = zalloc(sizeof(tts_context_t));

    ctx->user_loop = param->loop;
    if (param->loop) {
        ctx->user_asyncq.data = ctx;
        ret = uv_async_queue_init(param->loop, &ctx->user_asyncq, ai_tts_async_cb);
        if (ret < 0) {
            free(ctx);
            return NULL;
        }
    }

    ctx->plugin = plugin;
    ai_tts_map_params(ctx, param, &ctx->voice_param);
    ctx->engine = tts_plugin_init(plugin, &ctx->voice_param);
    if (ctx->engine == NULL) {
        free(ctx);
        AI_INFO("ai_tts_create_engine failed");
        return NULL;
    }

    env = ctx->plugin->get_env(ctx->engine);
    ctx->loop = env->loop;
    ctx->asyncq = env->asyncq;

    AI_INFO("ai_tts_create_engine:%p", ctx->loop);

    if (ctx->loop == NULL) {
        tts_plugin_uninit(plugin, ctx->engine, 1);
        free(ctx);
        return NULL;
    }

    return ctx;
}

int ai_tts_set_listener(tts_handle_t handle, tts_callback_t callback, void* cookie)
{
    tts_context_t* ctx = (tts_context_t*)handle;

    AI_INFO("ai_tts_set_listener:%p", ctx->asyncq);

    if (ctx == NULL || ctx->engine == NULL || ctx->asyncq == NULL)
        return -1;

    message_t* message = (message_t*)malloc(sizeof(message_t));
    message_data_listener_t* data = (message_data_listener_t*)calloc(1, sizeof(message_data_listener_t));
    data->ctx = ctx;
    data->cb = callback;
    data->cookie = cookie;
    message->message_id = TTS_MESSAGE_LISTENER;
    message->message_handler = ai_tts_set_listener_l;
    message->message_data = data;
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_tts_speak(tts_handle_t handle, const char* text, const tts_audio_info_t* audio_info)
{
    tts_context_t* ctx = (tts_context_t*)handle;

    AI_INFO("ai_tts_speak:%p", ctx->asyncq);

    if (ctx == NULL || ctx->engine == NULL || ctx->asyncq == NULL)
        return -EINVAL;

    message_t* message = (message_t*)malloc(sizeof(message_t));
    message_data_speak_t* data = (message_data_speak_t*)calloc(1, sizeof(message_data_speak_t));
    data->ctx = ctx;
    if (audio_info) {
        data->audio_info.version = audio_info->version;
        if (audio_info->format && strlen(audio_info->format) > 0) {
            data->audio_info.format = (char*)malloc(strlen(audio_info->format) + 1);
            strlcpy(data->audio_info.format, audio_info->format, strlen(audio_info->format) + 1);
        }
    }
    if (text) {
        data->text = (char*)malloc(strlen(text) + 1);
        strlcpy(data->text, text, strlen(text) + 1);
    }
    message->message_id = TTS_MESSAGE_START;
    message->message_handler = ai_tts_speak_l;
    message->message_data = data;
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_tts_stop(tts_handle_t handle)
{
    tts_context_t* ctx = (tts_context_t*)handle;

    AI_INFO("ai_tts_finish");

    if (ctx == NULL || ctx->asyncq == NULL)
        return -EINVAL;

    message_t* message = (message_t*)malloc(sizeof(message_t));
    message_data_finish_t* data = (message_data_finish_t*)calloc(1, sizeof(message_data_finish_t));
    data->ctx = ctx;
    message->message_id = TTS_MESSAGE_FINISH;
    message->message_handler = ai_tts_stop_l;
    message->message_data = data;
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_tts_is_busy(tts_handle_t handle)
{
    tts_context_t* ctx = (tts_context_t*)handle;

    AI_INFO("ai_tts_is_busy");

    if (ctx == NULL || ctx->handle == NULL || ctx->engine == NULL || ctx->asyncq == NULL)
        return -EINVAL;

    return 0;
}

static int ai_tts_send_close_message(tts_context_t* ctx)
{
    message_t* message = (message_t*)malloc(sizeof(message_t));
    message_data_close_t* data = (message_data_close_t*)calloc(1, sizeof(message_data_close_t));
    data->ctx = ctx;
    message->message_id = TTS_MESSAGE_CLOSE;
    message->message_handler = ai_tts_close_l;
    message->message_data = data;
    return uv_async_queue_send(ctx->asyncq, message);
}

static void ai_tts_uvasyncq_close_cb(uv_handle_t* handle)
{
    tts_context_t* ctx = uv_handle_get_data((const uv_handle_t*)handle);
    ai_tts_send_close_message(ctx);
    AI_INFO("ai_tts_uvasyncq_close_cb");
}

int ai_tts_close(tts_handle_t handle)
{
    tts_context_t* ctx = (tts_context_t*)handle;

    AI_INFO("ai_tts_close");

    if (ctx == NULL || ctx->asyncq == NULL)
        return -EINVAL;

    if (ctx->user_loop) {
        uv_handle_set_data((uv_handle_t*)&(ctx->user_asyncq), ctx);
        uv_close((uv_handle_t*)&(ctx->user_asyncq), ai_tts_uvasyncq_close_cb);
        return 0;
    } else {
        return ai_tts_send_close_message(ctx);
    }
}
