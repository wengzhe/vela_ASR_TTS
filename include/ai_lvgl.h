/****************************************************************************
 * frameworks/ai/src/asr/ai_lvgl.c
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
#ifndef __AI_LVGL_H
#define __AI_LVGL_H
/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <uv.h>

// include lvgl headers
#include <lvgl/lvgl.h>

/****************************************************************************
 * Public Types
 ****************************************************************************/
#define SCREEN_WIDTH    (lv_obj_get_width(lv_scr_act()))
#define SCREEN_HEIGHT   (lv_obj_get_height(lv_scr_act()))

#define DEMO_WIDTH (int32_t)((SCREEN_WIDTH * 0.55f))
#define DEMO_HEIGHT (int32_t)(DEMO_WIDTH)


typedef struct audio_text_s
{
    struct
    {
        lv_obj_t *root;
        lv_obj_t *title;
        lv_obj_t *clear_btnm;
        lv_obj_t *clear_btnm_text;
        lv_obj_t *voice_btntnm;
        lv_obj_t *voice_btntnm_text;
        lv_obj_t *textarea;
    }ui;
    
}audio_text_t;


/****************************************************************************
 * Public Functions
 ****************************************************************************/


#endif // __AI_LVGL_H
