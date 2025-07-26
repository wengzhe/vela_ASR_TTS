/****************************************************************************
 * frameworks/ai/src/conversation/plugin/ai_conversation_defs.h
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

#ifndef FRAMEWORKS_AI_CONVERSATION_DEFS_H_
#define FRAMEWORKS_AI_CONVERSATION_DEFS_H_

#include <uv.h>
#include <uv_async_queue.h>

typedef enum {
    conversation_engine_event_unknown,
    conversation_engine_event_start,
    conversation_engine_event_stop,
    conversation_engine_event_complete,
    conversation_engine_event_input_text,
    conversation_engine_event_audio, // Response audio
    conversation_engine_event_text, // Response text
    conversation_engine_event_error,
} conversation_engine_event_t;

typedef enum {
    conversation_engine_error_success = 0,
    conversation_engine_error_unknown,
    conversation_engine_error_network,
    conversation_engine_error_server,
    conversation_engine_error_cancelled,
} conversation_engine_error_t;

typedef struct conversation_engine_result {
    const char* result;
    int len;
    conversation_engine_error_t error_code;
} conversation_engine_result_t;

typedef struct conversation_engine_audio_info {
    int version;
    char audio_type[10]; // pcm16
    int sample_rate; // 16000
    int channels; // 1
    int sample_bit; // 16
} conversation_engine_audio_info_t;

typedef void (*conversation_engine_callback_t)(conversation_engine_event_t event, 
                                              const conversation_engine_result_t* result, 
                                              void* cookie);

typedef void (*conversation_engine_uvasyncq_cb_t)(uv_async_queue_t* asyncq, void* data);

typedef struct conversation_engine_init_params {
    uv_loop_t* loop;
    const char* api_key;
    conversation_engine_uvasyncq_cb_t cb;
    void* opaque;
} conversation_engine_init_params_t;

typedef struct conversation_engine_env_params {
    uv_loop_t* loop;
    const char* format;
    int force_format;
} conversation_engine_env_params_t;

#endif // FRAMEWORKS_AI_CONVERSATION_DEFS_H_ 