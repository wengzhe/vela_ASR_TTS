/****************************************************************************
 * frameworks/ai/src/asr/ai_asr.c
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

#include "ai_asr.h"
#include "ai_asr_internal.h"
#include "ai_common.h"
#include "ai_voice_plugin.h"

#define ASR_DEFAULT_SILENCE_TIMEOUT 3000
#define ASR_MIN_SILENCE_TIMEOUT 300
#define ASR_MAX_SILENCE_TIMEOUT 15000

/****************************************************************************
 * Private Types
 ****************************************************************************/

extern voice_plugin_t xiaoai_asr_engine_plugin;
extern voice_plugin_t volc_asr_engine_plugin;

typedef struct asr_context {
    voice_plugin_t* plugin;
    void* engine;
    void* handle; // recorder handle
    void* focus_handle;
    uv_loop_t* loop;
    uv_loop_t* user_loop;
    uv_async_queue_t* asyncq;
    uv_async_queue_t user_asyncq;
    uv_pipe_t* pipe;
    char* format;
    asr_callback_t cb;
    void* cookie;
    asr_state_t state;
    int is_send_finished;
    int is_closed;
    voice_init_params_t voice_param;
    char last_result[1024];
    int64_t last_result_time;
} asr_context_t;

typedef enum {
    ASR_MESSAGE_CREATE_ENGINE,
    ASR_MESSAGE_LISTENER,
    ASR_MESSAGE_START,
    ASR_MESSAGE_FINISH,
    ASR_MESSAGE_CANCEL,
    ASR_MESSAGE_IS_BUSY,
    ASR_MESSAGE_CLOSE,
    ASR_MESSAGE_CB
} message_id_t;

typedef int (*message_handler_t)(void* message_data);

typedef struct message_s {
    message_id_t message_id;
    message_handler_t message_handler;
    void* message_data;
} message_t;

typedef struct message_data_listener_s {
    asr_context_t* ctx;
    asr_callback_t cb;
    void* cookie;
} message_data_listener_t;

typedef struct message_data_start_s {
    asr_context_t* ctx;
    asr_audio_info_t audio_info;
} message_data_start_t;

typedef struct message_data_finish_s {
    asr_context_t* ctx;
} message_data_finish_t;

typedef struct message_data_cancel_s {
    asr_context_t* ctx;
} message_data_cancel_t;

typedef struct message_data_is_busy_s {
    asr_context_t* ctx;
} message_data_is_busy_t;

typedef struct message_data_close_s {
    asr_context_t* ctx;
} message_data_close_t;

typedef struct message_data_cb_s {
    asr_context_t* ctx;
    voice_event_t event;
    asr_result_t* result;
} message_data_cb_t;

static void ai_asr_voice_callback(voice_event_t event, const voice_result_t* result, void* cookie);
static int ai_asr_close_handler(asr_context_t* ctx);
static void ai_asr_send_callback(asr_context_t* ctx, voice_event_t event, asr_result_t* result);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int64_t ai_asr_gettime_relative(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void alloc_read_buffer(uv_handle_t* handle, size_t suggested_size,
    uv_buf_t* buf)
{
    buf->base = (char*)calloc(1, suggested_size);
    buf->len = suggested_size;
}

static void read_buffer_cb(uv_stream_t* client, ssize_t nread, const uv_buf_t* buf)
{
    asr_context_t* ctx = uv_handle_get_data((uv_handle_t*)client);
    ctx->plugin->write_audio(ctx->engine, buf->base, nread);
    static int count = 0;
    if (count % 20 == 0)
        AI_INFO("asr recorder read audio data: %d\n", nread);
    count++;
    free(buf->base);
}

static void ai_asr_send_error(asr_context_t* ctx, asr_error_t error)
{
    voice_result_t result;

    result.duration = 0;
    result.result = NULL;
    result.error_code = error;
    ai_asr_voice_callback(voice_event_error, &result, ctx);
}

static void media_recorder_prepare_connect_cb(void* cookie, int ret, void* obj)
{
    asr_context_t* ctx = cookie;

    if (ret < 0) {
        ai_asr_send_error(ctx, asr_error_media);
        AI_INFO("asr recorder prepare connect cb error:%d\n", ret);
        return;
    }

    ctx->pipe = (uv_pipe_t*)obj;
    uv_handle_set_data((uv_handle_t*)ctx->pipe, ctx);
    uv_read_start((uv_stream_t*)ctx->pipe, alloc_read_buffer, read_buffer_cb);
}

static void media_recorder_open_cb(void* cookie, int ret)
{
    asr_context_t* ctx = cookie;

    if (ret < 0)
        ai_asr_send_error(ctx, asr_error_media);
    AI_INFO("asr recorder open cb:%d", ret);
}

static void media_recorder_start_cb(void* cookie, int ret)
{
    asr_context_t* ctx = cookie;

    if (ret < 0)
        ai_asr_send_error(ctx, asr_error_media);
    AI_INFO("asr recorder start cb:%d", ret);
}

static void media_recorder_close_cb(void* cookie, int ret)
{
    asr_context_t* ctx = cookie;

    AI_INFO("asr recorder close cb:%d", ret);
    if (ctx->is_closed)
        ai_asr_close_handler(ctx);
    ctx->is_closed = true;
}

static void media_recorder_event_callback(void* cookie, int event, int ret,
    const char* extra)
{
    asr_context_t* ctx = cookie;

    if (ret < 0)
        ai_asr_send_error(ctx, asr_error_media);

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

    AI_INFO("asr recorder event callback event:%d ret:%d", event, ret);
}

static void ai_asr_destroy_engine(asr_context_t* ctx)
{
    if (ctx->format) {
        free(ctx->format);
        ctx->format = NULL;
    }

    if (ctx->engine) {
        voice_plugin_uninit(ctx->plugin, ctx->engine, 0);
        ctx->engine = NULL;
    }

    free(ctx);
    ctx = NULL;
}

static void ai_asr_uvasyncq_close_cb(uv_handle_t* handle)
{
    asr_context_t* ctx = uv_handle_get_data((const uv_handle_t*)handle);

    ai_asr_destroy_engine(ctx);
    AI_INFO("ai_asr_uvasyncq_close_cb");
}

static void ai_asr_close_async(asr_context_t* ctx)
{
    uv_handle_set_data((uv_handle_t*)&(ctx->user_asyncq), ctx);
    uv_close((uv_handle_t*)&(ctx->user_asyncq), ai_asr_uvasyncq_close_cb);
}

static int ai_asr_close_handler(asr_context_t* ctx)
{
    int ret = 0;

    if (ctx == NULL)
        return -EINVAL;

    ai_asr_send_callback(ctx, voice_event_closed, NULL);

    AI_INFO("ai_asr_close_handler");
    return ret;
}

static int ai_asr_finish_handler(asr_context_t* ctx)
{
    int ret = 0;

    if (ctx == NULL)
        return -EINVAL;

    if (ctx->handle != NULL) {
        ret = media_uv_recorder_close(ctx->handle, media_recorder_close_cb);
        if (ret < 0)
            AI_INFO("close recorder failed:%d", ret);
        ctx->handle = NULL;
    }

    if (ctx->focus_handle != NULL) {
        media_focus_abandon(ctx->focus_handle);
        ctx->focus_handle = NULL;
    }

    if (ctx->engine != NULL)
        ret = ctx->plugin->finish(ctx->engine);

    AI_INFO("ai_asr_finish_handler");

    return ret;
}

static void ai_asr_focus_callback(int suggestion, void* cookie)
{
    asr_context_t* ctx = cookie;

    if (suggestion != MEDIA_FOCUS_PLAY) {
        ai_asr_finish_handler(ctx);
        ai_asr_voice_callback(voice_event_complete, NULL, ctx);
    }

    AI_INFO("asr recorder focus suggestion:%d", suggestion);
}

static int ai_asr_init_recorder(asr_context_t* ctx)
{
    const char* format = ctx->format;
    int init_suggestion;
    char* stream = "cap";
    void* handle = NULL;

    ctx->focus_handle = media_focus_request(&init_suggestion, MEDIA_SCENARIO_TTS, ai_asr_focus_callback, ctx);
    if (init_suggestion != MEDIA_FOCUS_PLAY && ctx->focus_handle) {
        AI_INFO("asr recorder focus failed");
        media_focus_abandon(ctx->focus_handle);
        ctx->focus_handle = NULL;
        goto failed;
    }

    handle = media_uv_recorder_open(ctx->loop, stream, media_recorder_open_cb, ctx);
    if (handle == NULL) {
        AI_INFO("asr recorder open failed");
        goto failed;
    }

    int ret = media_uv_recorder_listen(handle, media_recorder_event_callback);
    if (ret < 0) {
        AI_INFO("asr recorder listen failed");
        media_uv_recorder_close(handle, media_recorder_close_cb);
        goto failed;
    }

    ret = media_uv_recorder_prepare(handle, NULL, format,
        media_recorder_prepare_connect_cb, NULL, NULL);
    if (ret < 0) {
        AI_INFO("asr recorder prepare failed");
        media_uv_recorder_close(handle, media_recorder_close_cb);
        goto failed;
    }

    ctx->handle = handle;
    AI_INFO("ai_asr_init_recorder %p\n", ctx->handle);

    return 0;
failed:
    return -EPERM;
}

static int ai_asr_callback_l(void* message_data)
{
    message_data_cb_t* data = (message_data_cb_t*)message_data;
    asr_context_t* ctx = data->ctx;
    asr_event_t event = data->event;
    asr_result_t* asr_result = data->result;

    if (ctx->cb)
        ctx->cb(event, asr_result, ctx->cookie);

    if (asr_result) {
        free(asr_result->result);
        free(asr_result);
    }

    if (event == asr_event_closed) {
        if (ctx->user_loop)
            ai_asr_close_async(ctx);
        else
            ai_asr_destroy_engine(ctx);
    }

    return 0;
}

static void ai_asr_send_callback(asr_context_t* ctx, voice_event_t event, asr_result_t* result)
{
    message_data_cb_t* cb = (message_data_cb_t*)malloc(sizeof(message_data_cb_t));
    cb->ctx = ctx;
    cb->event = event;
    cb->result = result;
    if (ctx->user_loop) {
        message_t* message = (message_t*)malloc(sizeof(message_t));
        message->message_id = ASR_MESSAGE_CB;
        message->message_handler = ai_asr_callback_l;
        message->message_data = cb;
        uv_async_queue_send(&(ctx->user_asyncq), message);
    } else {
        ai_asr_callback_l(cb);
        free(cb);
    }
}

static void ai_asr_voice_callback(voice_event_t event, const voice_result_t* result, void* cookie)
{
    asr_context_t* ctx = cookie;
    asr_result_t* asr_result = NULL;

    if (ctx->cb == NULL)
        return;

    if (ctx->is_send_finished || ctx->state == ASR_STATE_CLOSE)
        return;

    if (result) {
        asr_result = (asr_result_t*)malloc(sizeof(asr_result_t));
        if (result->result != NULL) {
            asr_result->result = (char*)malloc(strlen(result->result) + 1);
            strlcpy(asr_result->result, result->result, strlen(result->result) + 1);
        } else
            asr_result->result = NULL;
        asr_result->duration = result->duration;
        if (result->error_code != 0)
            asr_result->error_code = asr_error_failed;
        else
            asr_result->error_code = 0;
        AI_INFO("ai_asr_voice_callback:%s", result->result);

        if (asr_result->result != NULL) {
                    if (strcmp(ctx->last_result, asr_result->result) == 0 && ctx->last_result_time != 0 && (ai_asr_gettime_relative() - ctx->last_result_time) > ctx->voice_param.silence_timeout * 1000) {
            AI_INFO("ai_asr_voice_callback timeout: %s %d", asr_result->result, ctx->voice_param.silence_timeout);
                free(asr_result->result);
                free(asr_result);
                ai_asr_voice_callback(voice_event_complete, NULL, ctx);
                ctx->is_send_finished = true;
                return;
            }

            if (strcmp(ctx->last_result, asr_result->result) || ctx->last_result_time == 0) {
                strlcpy(ctx->last_result, asr_result->result, sizeof(ctx->last_result));
                ctx->last_result_time = ai_asr_gettime_relative();
                AI_INFO("ai_asr_voice_callback first time:%s", ctx->last_result);
            }
        }
    }

    if (voice_event_complete == event || voice_event_error == event) {
        ai_asr_finish_handler(ctx);
        AI_INFO("ai_asr_voice_callback complete or error");
        ctx->is_send_finished = true;
    }

    ai_asr_send_callback(ctx, event, asr_result);
}

static void ai_asr_async_cb(uv_async_queue_t* handle, void* data)
{
    AI_INFO("ai_asr_async_cb");

    message_t* message = (message_t*)data;

    if (message->message_handler)
        message->message_handler(message->message_data);

    free(message->message_data);
    free(message);
}

static int ai_asr_map_params(asr_context_t* ctx, const asr_init_params_t* in_param, const ai_auth_t* auth,
    voice_init_params_t* out_param)
{
    if (!auth)
        return -EINVAL;

    out_param->loop = in_param->loop;
    out_param->locate = in_param->locate ?: "CN";
    out_param->rec_mode = in_param->rec_mode ?: "short";
    out_param->language = in_param->language ?: "zh-CN";
    if (in_param->silence_timeout <= ASR_MAX_SILENCE_TIMEOUT && in_param->silence_timeout >= ASR_MIN_SILENCE_TIMEOUT)
        out_param->silence_timeout = in_param->silence_timeout;
    else if (in_param->silence_timeout > ASR_MAX_SILENCE_TIMEOUT)
        out_param->silence_timeout = ASR_MAX_SILENCE_TIMEOUT;
    else if (in_param->silence_timeout < ASR_MIN_SILENCE_TIMEOUT && in_param->silence_timeout != 0)
        out_param->silence_timeout = ASR_MIN_SILENCE_TIMEOUT;
    else
        out_param->silence_timeout = ASR_DEFAULT_SILENCE_TIMEOUT;
    out_param->cb = ai_asr_async_cb;
    out_param->opaque = ctx;
    if (auth->engine_type == asr_engine_type_volc) {
        ai_volc_auth_t* volc_auth = (ai_volc_auth_t*)auth->auth;
        if (volc_auth) {
            if (volc_auth->app_id == NULL || volc_auth->app_key == NULL || strlen(volc_auth->app_id) == 0
                || strlen(volc_auth->app_key) == 0)
                return -EINVAL;

            out_param->app_id = volc_auth->app_id;
            out_param->app_key = volc_auth->app_key;
        } else {
            return -EINVAL;
        }
    }

    return 0;
}

static int ai_asr_set_listener_l(void* message_data)
{
    message_data_listener_t* data = (message_data_listener_t*)message_data;
    asr_context_t* ctx = data->ctx;
    asr_callback_t callback = data->cb;
    void* cookie = data->cookie;

    if (ctx == NULL || ctx->engine == NULL)
        return -1;

    ctx->cb = callback;
    ctx->cookie = cookie;

    AI_INFO("ai_asr_set_listener_l");

    return ctx->plugin->event_cb(ctx->engine, ai_asr_voice_callback, ctx);
}

static int ai_asr_create_format(asr_context_t* ctx, const char* format)
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

static int ai_asr_start_l(void* message_data)
{
    message_data_start_t* data = (message_data_start_t*)message_data;
    asr_context_t* ctx = data->ctx;
    const asr_audio_info_t* audio_info = &data->audio_info;
    voice_env_params_t* env;
    int ret = 0;

    AI_INFO("ai_asr_start_l before");

    if (ctx == NULL || ctx->engine == NULL)
        return -EINVAL;

    if (ctx->state == ASR_STATE_START) {
        AI_INFO("AIEngine state is error:%d\n", ctx->state);
        return 0;
    }

    env = ctx->plugin->get_env(ctx->engine);
    if (audio_info && audio_info->format && !env->force_format) {
        ret = ai_asr_create_format(ctx, audio_info->format);
        free(audio_info->format);
    } else {
        ret = ai_asr_create_format(ctx, env->format);
    }
    if (ret < 0)
        return ret;

    memset(ctx->last_result, 0, sizeof(ctx->last_result));
    ctx->last_result_time = 0;
    ctx->state = ASR_STATE_START;
    ctx->is_send_finished = false;
    ctx->is_closed = false;

    ret = ctx->plugin->start(ctx->engine, NULL);
    if (ret < 0)
        goto failed;

    ret = ai_asr_init_recorder(ctx);
    if (ret < 0)
        return ret;

    ret = media_uv_recorder_start(ctx->handle, media_recorder_start_cb, ctx);
    if (ret < 0)
        goto failed;

    ai_asr_voice_callback(voice_event_start, NULL, ctx);

    AI_INFO("ai_asr_start_l");

    return ret;
failed:
    AI_INFO("ai_asr_start_l failed");
    media_uv_recorder_close(ctx->handle, media_recorder_close_cb);
    ctx->handle = NULL;
    return ret;
}

static int ai_asr_finish_l(void* message_data)
{
    message_data_finish_t* data = (message_data_finish_t*)message_data;
    asr_context_t* ctx = data->ctx;
    int ret;

    AI_INFO("ai_asr_finish_l");

    if (ctx->state != ASR_STATE_START) {
        AI_INFO("AIEngine state is error:%d\n", ctx->state);
        return 0;
    }
    ctx->state = ASR_STATE_FINISH;

    ret = ai_asr_finish_handler(data->ctx);
    ai_asr_voice_callback(voice_event_complete, NULL, data->ctx);

    return ret;
}

static int ai_asr_cancel_l(void* message_data)
{
    message_data_cancel_t* data = (message_data_cancel_t*)message_data;
    asr_context_t* ctx = data->ctx;
    int ret;

    AI_INFO("ai_asr_cancel_l");

    if (ctx->state != ASR_STATE_START) {
        AI_INFO("AIEngine state is error:%d\n", ctx->state);
        return 0;
    }
    ctx->state = ASR_STATE_CANCEL;

    ctx->is_send_finished = true;
    ret = ai_asr_finish_handler(data->ctx);

    return ret;
}

static int ai_asr_close_l(void* message_data)
{
    message_data_close_t* data = (message_data_close_t*)message_data;
    asr_context_t* ctx = data->ctx;

    AI_INFO("ai_asr_close_l");

    if (ctx->state == ASR_STATE_CLOSE || ctx->state == ASR_STATE_START) {
        AI_INFO("AIEngine state is error:%d\n", ctx->state);
        return 0;
    }
    ctx->state = ASR_STATE_CLOSE;

    if (ctx->is_closed)
        ai_asr_close_handler(ctx);
    else
        ctx->is_closed = true;

    return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

asr_handle_t ai_asr_create_engine_l(const asr_init_params_t* param, const ai_auth_t* auth)
{
    asr_context_t* ctx;
    voice_plugin_t* plugin;
    voice_env_params_t* env;
    ai_auth_t ai_auth;
    ai_volc_auth_t volc_auth;
    int ret;

    if (param == NULL)
        return NULL;

    if (auth) {
        if (auth->engine_type == asr_engine_type_volc)
            plugin = &volc_asr_engine_plugin;
        else {
            AI_INFO("unknown engine type: %d", auth->engine_type);
            return NULL;
        }
    } else {
        // todo: 从安全存储中获取 auth
        plugin = &volc_asr_engine_plugin;

        volc_auth.app_id = "3306859263";
        volc_auth.app_key = "LyWxL1O5wV4UMgqhSgjU6QnEcV_HJIaD";
        ai_auth.engine_type = asr_engine_type_volc;
        ai_auth.auth = &volc_auth;
        auth = &ai_auth;
    }

    ctx = zalloc(sizeof(asr_context_t));
    if (ctx == NULL)
        return NULL;
    ctx->is_closed = true;
    ctx->state = ASR_STATE_INIT;

    ctx->user_loop = param->loop;
    if (param->loop) {
        ctx->user_asyncq.data = ctx;
        ret = uv_async_queue_init(param->loop, &ctx->user_asyncq, ai_asr_async_cb);
        if (ret < 0) {
            free(ctx);
            return NULL;
        }
    }

    ctx->plugin = plugin;
    ret = ai_asr_map_params(ctx, param, auth, &ctx->voice_param);
    if (ret < 0) {
        free(ctx);
        AI_INFO("ai_asr_create_engine auth error");
        return NULL;
    }
    ctx->engine = voice_plugin_init(plugin, &ctx->voice_param);
    if (ctx->engine == NULL) {
        free(ctx);
        AI_INFO("ai_asr_create_engine failed");
        return NULL;
    }

    env = ctx->plugin->get_env(ctx->engine);
    ctx->loop = env->loop;
    ctx->asyncq = env->asyncq;

    AI_INFO("ai_asr_create_engine:%p", ctx->loop);

    if (ctx->loop == NULL) {
        voice_plugin_uninit(plugin, ctx->engine, 1);
        free(ctx);
        return NULL;
    }

    return ctx;
}

asr_handle_t ai_asr_create_engine(const asr_init_params_t* param)
{
    AI_INFO("ai_asr_create_engine");
    return ai_asr_create_engine_l(param, NULL);
}

asr_handle_t ai_asr_create_engine_with_auth(const asr_init_params_t* param, const ai_auth_t* auth)
{
    if (auth == NULL || auth->auth == NULL)
        return NULL;

    AI_INFO("ai_asr_create_engine_with_auth type: %d", auth->engine_type);
    return ai_asr_create_engine_l(param, auth);
}

int ai_asr_set_listener(asr_handle_t handle, asr_callback_t callback, void* cookie)
{
    asr_context_t* ctx = (asr_context_t*)handle;

    AI_INFO("ai_asr_set_listener:%p", ctx->asyncq);

    if (ctx == NULL || ctx->engine == NULL || ctx->asyncq == NULL)
        return -1;

    message_t* message = (message_t*)malloc(sizeof(message_t));
    message_data_listener_t* data = (message_data_listener_t*)calloc(1, sizeof(message_data_listener_t));
    data->ctx = ctx;
    data->cb = callback;
    data->cookie = cookie;
    message->message_id = ASR_MESSAGE_LISTENER;
    message->message_handler = ai_asr_set_listener_l;
    message->message_data = data;
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_asr_start(asr_handle_t handle, const asr_audio_info_t* audio_info)
{
    asr_context_t* ctx = (asr_context_t*)handle;

    AI_INFO("ai_asr_start:%p", ctx->asyncq);

    if (ctx == NULL || ctx->engine == NULL || ctx->asyncq == NULL)
        return -EINVAL;

    message_t* message = (message_t*)malloc(sizeof(message_t));
    message_data_start_t* data = (message_data_start_t*)calloc(1, sizeof(message_data_start_t));
    data->ctx = ctx;
    if (audio_info) {
        data->audio_info.version = audio_info->version;
        if (audio_info->format && strlen(audio_info->format) > 0) {
            data->audio_info.format = (char*)malloc(strlen(audio_info->format) + 1);
            strlcpy(data->audio_info.format, audio_info->format, strlen(audio_info->format) + 1);
        }
    }
    message->message_id = ASR_MESSAGE_START;
    message->message_handler = ai_asr_start_l;
    message->message_data = data;
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_asr_finish(asr_handle_t handle)
{
    asr_context_t* ctx = (asr_context_t*)handle;

    AI_INFO("ai_asr_finish");

    if (ctx == NULL || ctx->asyncq == NULL)
        return -EINVAL;

    message_t* message = (message_t*)malloc(sizeof(message_t));
    message_data_finish_t* data = (message_data_finish_t*)calloc(1, sizeof(message_data_finish_t));
    data->ctx = ctx;
    message->message_id = ASR_MESSAGE_FINISH;
    message->message_handler = ai_asr_finish_l;
    message->message_data = data;
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_asr_cancel(asr_handle_t handle)
{
    asr_context_t* ctx = (asr_context_t*)handle;

    AI_INFO("ai_asr_cancel");

    if (ctx == NULL || ctx->asyncq == NULL)
        return -EINVAL;

    message_t* message = (message_t*)malloc(sizeof(message_t));
    message_data_cancel_t* data = (message_data_cancel_t*)calloc(1, sizeof(message_data_cancel_t));
    data->ctx = ctx;
    message->message_id = ASR_MESSAGE_CANCEL;
    message->message_handler = ai_asr_cancel_l;
    message->message_data = data;
    return uv_async_queue_send(ctx->asyncq, message);
}

int ai_asr_is_busy(asr_handle_t handle)
{
    asr_context_t* ctx = (asr_context_t*)handle;

    AI_INFO("ai_asr_is_busy");

    if (ctx == NULL || ctx->handle == NULL || ctx->engine == NULL || ctx->asyncq == NULL)
        return -EINVAL;

    return 0;
}

int ai_asr_close(asr_handle_t handle)
{
    asr_context_t* ctx = (asr_context_t*)handle;

    AI_INFO("ai_asr_close");

    if (ctx == NULL || ctx->asyncq == NULL)
        return -EINVAL;

    message_t* message = (message_t*)malloc(sizeof(message_t));
    message_data_close_t* data = (message_data_close_t*)calloc(1, sizeof(message_data_close_t));
    data->ctx = ctx;
    message->message_id = ASR_MESSAGE_CLOSE;
    message->message_handler = ai_asr_close_l;
    message->message_data = data;

    return uv_async_queue_send(ctx->asyncq, message);
}

asr_state_t ai_asr_get_state(asr_handle_t handle)
{
    asr_context_t* ctx = (asr_context_t*)handle;
    return ctx->state;
}