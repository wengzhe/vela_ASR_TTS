/****************************************************************************
 * frameworks/ai/include/ai_tts.h
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

#ifndef FRAMEWORKS_AI_INCLUDE_AI_TTS_H
#define FRAMEWORKS_AI_INCLUDE_AI_TTS_H

#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Included Files
 ****************************************************************************/

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef void* tts_handle_t;

typedef enum {
    tts_engine_type_volc
} tts_engine_type;

typedef struct tts_init_params {
    int version;
    uv_loop_t* loop;
    const char* language; // zh-CN
    int slience_timeout; // 2000ms
    const char* app_id;
    const char* app_key;
    tts_engine_type engine_type;
} tts_init_params_t;

typedef enum {
    tts_event_unkonwn,
    tts_event_start,
    tts_event_stop,
    tts_event_complete,
    tts_event_data,
    tts_event_error,
} tts_event_t;

typedef enum {
    tts_error_unknown,
    tts_error_failed,
    tts_error_media,
    tts_error_destroyed,
} tts_error_t;

typedef struct tts_result {
    char* result;
    int len;
    tts_error_t error_code;
} tts_result_t;

typedef struct tts_audio_info {
    int version;
    char* format;
} tts_audio_info_t;

typedef void (*tts_callback_t)(tts_event_t event, const tts_result_t* result, void* cookie);

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief Create ai tts engine.
 * @param[in] param tts init params
 * @return tts engine
 */
tts_handle_t ai_tts_create_engine(const tts_init_params_t* param);

/**
 * @brief Set tts engine listener.
 * @param[in] handle tts handle
 * @param[in] callback tts event callback
 * @param[in] cookie callback cookie
 * @return 0 on success, otherwise failed
 */
int ai_tts_set_listener(tts_handle_t handle, tts_callback_t callback, void* cookie);

/**
 * @brief Start tts engine.
 * @param[in] handle tts handle
 * @param[in] audio_info tts audio info
 * @return 0 on success, otherwise failed
 */
int ai_tts_speak(tts_handle_t handle, const char* text, const tts_audio_info_t* audio_info);

/**
 * @brief Finish tts engine.
 * @param[in] handle tts handle
 * @return 0 on success, otherwise failed
 */
int ai_tts_stop(tts_handle_t handle);

/**
 * @brief Destroy tts engine.
 * @param[in] handle tts handle
 * @return 0 on success, otherwise failed
 */
int ai_tts_is_busy(tts_handle_t handle);

/**
 * @brief Destroy tts engine.
 * @param[in] handle tts handle
 * @return 0 on success, otherwise failed
 */
int ai_tts_close(tts_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* FRAMEWORKS_AI_INCLUDE_AI_TTS_H */
