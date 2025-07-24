/****************************************************************************
 * frameworks/ai/src/conversation/plugin/ai_conversation_plugin.c
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "ai_conversation_plugin.h"
#include "ai_common.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void* conversation_plugin_init(conversation_engine_plugin_t* plugin, 
                              const conversation_engine_init_params_t* param)
{
    void* engine;
    
    if (!plugin || !param) {
        AI_INFO("Invalid plugin or param");
        return NULL;
    }
    
    AI_INFO("Initializing conversation plugin: %s", plugin->name);
    
    engine = calloc(1, plugin->priv_size);
    if (!engine) {
        AI_INFO("Failed to allocate memory for conversation engine");
        return NULL;
    }
    
    if (plugin->init && plugin->init(engine, param) < 0) {
        AI_INFO("Failed to initialize conversation plugin: %s", plugin->name);
        free(engine);
        return NULL;
    }
    
    AI_INFO("Conversation plugin initialized successfully: %s", plugin->name);
    return engine;
}

void conversation_plugin_uninit(conversation_engine_plugin_t* plugin, void* engine, int sync)
{
    if (!plugin || !engine) {
        return;
    }
    
    AI_INFO("Uninitializing conversation plugin: %s", plugin->name);
    
    if (plugin->uninit) {
        plugin->uninit(engine);
    }
    
    free(engine);
    
    AI_INFO("Conversation plugin uninitialized: %s", plugin->name);
} 