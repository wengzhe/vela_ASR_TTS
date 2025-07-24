/****************************************************************************
 * frameworks/ai/src/voice/ai_voice_defs.h
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

#ifndef FRAMEWORKS_AI_VOICE_DEFS_H_
#define FRAMEWORKS_AI_VOICE_DEFS_H_
#include <uv.h>
#include <uv_async_queue.h>

typedef enum {
    voice_event_unknown,
    voice_event_start,
    voice_event_cancel,
    voice_event_result,
    voice_event_complete,
    voice_event_error,
    voice_event_closed,
} voice_event_t;

typedef enum {
    voice_error_success = 0,
    voice_error_unknown,
    voice_error_network,
    voice_error_auth,
    voice_error_listen_timeout,
    voice_error_asr_timeout,
    voice_error_tts_timeout,
    voice_error_content_too_long,
    voice_error_too_many_devices,
} voice_error_t;

typedef struct voice_result {
    const char* result;
    int duration;
    voice_error_t error_code;
} voice_result_t;

typedef struct voice_audio_info {
    int version;
    char audio_type[10]; // pcm opus
    int sample_rate; // 16000
    int channels; // 1
    int sample_bit; // 16
} voice_audio_info_t;

typedef void (*voice_callback_t)(voice_event_t event, const voice_result_t* result, void* cookie);
typedef void (*ai_uvasyncq_cb_t)(uv_async_queue_t* asyncq, void* data);

typedef struct voice_init_params {
    uv_loop_t* loop;
    const char* locate;
    const char* rec_mode;
    const char* language;
    int silence_timeout;
    const char* app_id;
    const char* app_key;
    ai_uvasyncq_cb_t cb;
    void* opaque;
} voice_init_params_t;

typedef struct voice_env_params {
    uv_loop_t* loop;
    const char* format;
    int force_format;
    uv_async_queue_t* asyncq;
} voice_env_params_t;

#endif // FRAMEWORKS_AI_VOICE_DEFS_H_
