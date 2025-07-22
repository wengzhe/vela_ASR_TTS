/****************************************************************************
 * frameworks/ai/utils/ai_common.h
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

#ifndef FRAMEWORKS_AI_UTILS_AI_COMMON_H
#define FRAMEWORKS_AI_UTILS_AI_COMMON_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <syslog.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Debug log definition. */
#define AI_LOG(level, fmt, args...) \
    syslog(level, "[AI][%s:%d] " fmt, __func__, __LINE__, ##args)

#if defined(CONFIG_AI_LOG_DEBUG)
#define AI_DEBUG(fmt, args...) AI_LOG(LOG_DEBUG, fmt, ##args)
#define AI_INFO(fmt, args...) AI_LOG(LOG_INFO, fmt, ##args)
#define AI_WARN(fmt, args...) AI_LOG(LOG_WARNING, fmt, ##args)
#define AI_ERR(fmt, args...) AI_LOG(LOG_ERR, fmt, ##args)
#elif defined(CONFIG_AI_LOG_INFO)
#define AI_DEBUG(fmt, args...)
#define AI_INFO(fmt, args...) AI_LOG(LOG_INFO, fmt, ##args)
#define AI_WARN(fmt, args...) AI_LOG(LOG_WARNING, fmt, ##args)
#define AI_ERR(fmt, args...) AI_LOG(LOG_ERR, fmt, ##args)
#elif defined(CONFIG_AI_LOG_WARN)
#define AI_DEBUG(fmt, args...)
#define AI_INFO(fmt, args...)
#define AI_WARN(fmt, args...) AI_LOG(LOG_WARNING, fmt, ##args)
#define AI_ERR(fmt, args...) AI_LOG(LOG_ERR, fmt, ##args)
#elif defined(CONFIG_AI_LOG_ERR)
#define AI_DEBUG(fmt, args...)
#define AI_INFO(fmt, args...)
#define AI_WARN(fmt, args...)
#define AI_ERR(fmt, args...) AI_LOG(LOG_ERR, fmt, ##args)
#else
#define AI_DEBUG(fmt, args...)
#define AI_INFO(fmt, args...)
#define AI_WARN(fmt, args...)
#define AI_ERR(fmt, args...)
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#endif /* FRAMEWORKS_AI_UTILS_AI_COMMON_H */
