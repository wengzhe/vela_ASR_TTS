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

#ifndef FRAMEWORKS_AI_INCLUDE_AI_DEFS_H
#define FRAMEWORKS_AI_INCLUDE_AI_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ai_auth {
    int version;
    int engine_type;
    void* auth;
} ai_auth_t;

typedef struct ai_volc_auth {
    int version;
    const char* app_id;
    const char* app_key;
} ai_volc_auth_t;

#ifdef __cplusplus
}
#endif

#endif /* FRAMEWORKS_AI_INCLUDE_AI_DEFS_H */
