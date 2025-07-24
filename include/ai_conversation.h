/****************************************************************************
 * frameworks/ai/include/ai_conversation.h
 *
 * Copyright (C) 2020 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FRAMEWORKS_AI_INCLUDE_AI_CONVERSATION_H
#define FRAMEWORKS_AI_INCLUDE_AI_CONVERSATION_H

#include <ai_defs.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef void* conversation_handle_t;

typedef enum {
    conversation_engine_type_volc,
} conversation_engine_type;

typedef struct conversation_init_params {
    int version;
    uv_loop_t* loop;
    const char* language; // zh-CN
    const char* voice; // zh_xiaoyun_bigtts
    const char* instructions; // system prompt
    int timeout; // 30000ms
    const char* app_id;
    const char* app_key;
    conversation_engine_type engine_type;
} conversation_init_params_t;

typedef enum {
    conversation_event_unknown,
    conversation_event_connected,
    conversation_event_session_created,
    conversation_event_listening,
    conversation_event_processing,
    conversation_event_speaking,
    conversation_event_user_transcript,
    conversation_event_response_audio,
    conversation_event_response_text,
    conversation_event_complete,
    conversation_event_error,
    conversation_event_disconnected,
} conversation_event_t;

typedef enum {
    conversation_error_unknown,
    conversation_error_failed,
    conversation_error_network,
    conversation_error_auth,
    conversation_error_timeout,
    conversation_error_audio_format,
} conversation_error_t;

typedef struct conversation_result {
    const char* text;
    const void* audio_data;
    int audio_length;
    int duration;
    conversation_error_t error_code;
} conversation_result_t;

typedef struct conversation_audio_info {
    int version;
    char* format; // pcm16
    int sample_rate; // 16000
    int channels; // 1
    int sample_bit; // 16
} conversation_audio_info_t;

typedef void (*conversation_callback_t)(conversation_event_t event, 
                                       const conversation_result_t* result, 
                                       void* cookie);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief Create ai conversation engine.
 * @param[in] param conversation init params
 * @return conversation engine handle
 */
conversation_handle_t ai_conversation_create_engine(const conversation_init_params_t* param);



/**
 * @brief Set conversation engine listener.
 * @param[in] handle conversation handle
 * @param[in] callback conversation event callback
 * @param[in] cookie callback cookie
 * @return 0 on success, otherwise failed
 */
int ai_conversation_set_listener(conversation_handle_t handle, 
                                conversation_callback_t callback, 
                                void* cookie);

/**
 * @brief Start conversation engine.
 * @param[in] handle conversation handle
 * @param[in] audio_info conversation audio info
 * @return 0 on success, otherwise failed
 */
int ai_conversation_start(conversation_handle_t handle, 
                         const conversation_audio_info_t* audio_info);

/**
 * @brief Finish current audio input.
 * @param[in] handle conversation handle
 * @return 0 on success, otherwise failed
 */
int ai_conversation_finish(conversation_handle_t handle);

/**
 * @brief Cancel current conversation.
 * @param[in] handle conversation handle
 * @return 0 on success, otherwise failed
 */
int ai_conversation_cancel(conversation_handle_t handle);

/**
 * @brief Check if conversation engine is busy.
 * @param[in] handle conversation handle
 * @return 1 if busy, 0 if idle, negative if error
 */
int ai_conversation_is_busy(conversation_handle_t handle);

/**
 * @brief Close conversation engine.
 * @param[in] handle conversation handle
 * @return 0 on success, otherwise failed
 */
int ai_conversation_close(conversation_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* FRAMEWORKS_AI_INCLUDE_AI_CONVERSATION_H */ 