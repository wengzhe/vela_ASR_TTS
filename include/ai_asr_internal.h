/****************************************************************************
 * frameworks/ai/include/ai_asr.h
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

#ifndef FRAMEWORKS_AI_INCLUDE_AI_ASR_INTERNAL_H
#define FRAMEWORKS_AI_INCLUDE_AI_ASR_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include "ai_asr.h"

/****************************************************************************
 * Public Types
 ****************************************************************************/

typedef enum {
    ASR_STATE_INIT,
    ASR_STATE_START,
    ASR_STATE_FINISH,
    ASR_STATE_CANCEL,
    ASR_STATE_CLOSE
} asr_state_t;

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/**
 * @brief Create ai asr engine.
 * @param[in] param asr init params
 * @return asr engine
 */
asr_handle_t ai_asr_create_engine(const asr_init_params_t* param);

/**
 * @brief Get engine state.
 * @param[in] handle asr handle
 * @return asr state
 */
asr_state_t ai_asr_get_state(asr_handle_t handle);

#ifdef __cplusplus
}
#endif

#endif /* FRAMEWORKS_AI_INCLUDE_AI_ASR_INTERNAL_H */
