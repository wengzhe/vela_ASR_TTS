/****************************************************************************
 * frameworks/ai/src/tts_plugin/ai_tts_defs.h
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

#ifndef FRAMEWORKS_AI_TTS_PLUGIN_DEFS_H_
#define FRAMEWORKS_AI_TTS_PLUGIN_DEFS_H_
#include <uv.h>
#include <uv_async_queue.h>

typedef enum {
    tts_engine_event_unkonwn,
    tts_engine_event_start,
    tts_engine_event_stop,
    tts_engine_event_complete,
    tts_engine_event_result,
    tts_engine_event_error,
} tts_engine_event_t;

typedef enum {
    tts_engine_error_success = 0,
    tts_engine_error_unkonwn,
    tts_engine_error_network,
    tts_engine_error_auth,
    tts_engine_error_listen_timeout,
    tts_engine_error_asr_timeout,
    tts_engine_error_tts_timeout,
    tts_engine_error_content_too_long,
    tts_engine_error_too_many_devices,
} tts_engine_error_t;

typedef struct tts_engine_result {
    const char* result;
    int len;
    tts_engine_error_t error_code;
} tts_engine_result_t;

typedef struct tts_engine_audio_info {
    int version;
    char audio_type[10]; // pcm opus
    int sample_rate; // 16000
    int channels; // 1
    int sample_bit; // 16
} tts_engine_audio_info_t;

typedef void (*tts_engine_callback_t)(tts_engine_event_t event, const tts_engine_result_t* result, void* cookie);
typedef void (*tts_engine_uvasyncq_cb_t)(uv_async_queue_t* asyncq, void* data);

typedef struct tts_engine_init_params {
    uv_loop_t* loop;
    const char* language;
    int slience_timeout;
    const char* app_id;
    const char* app_key;
    tts_engine_uvasyncq_cb_t cb;
    void* opaque;
} tts_engine_init_params_t;

typedef struct tts_engine_env_params {
    uv_loop_t* loop;
    const char* format;
    int force_format;
    uv_async_queue_t* asyncq;
} tts_engine_env_params_t;

#endif // FRAMEWORKS_AI_TTS_ENGINE_DEFS_H_
