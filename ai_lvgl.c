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

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <unistd.h>
#include <uv.h>

// include lvgl headers
#include <lvgl/lvgl.h>

#include "ai_lvgl.h"
#include "ai_tool.h"
// #include "MiSans_Normal.h"
/****************************************************************************
 * Private Types
 ****************************************************************************/


audio_text_t g_audio_text;
aitool_t aitool;
lv_timer_t *press_timer = NULL; // Define the timer pointer
lv_font_t *font;
lv_obj_t* mic_obj;
extern const lv_image_dsc_t mic;
/****************************************************************************
 * Private Functions
 ****************************************************************************/
static void lv_nuttx_uv_loop(uv_loop_t* loop, lv_nuttx_result_t* result)
{
    lv_nuttx_uv_t uv_info;
    void* data;

    uv_loop_init(loop);

    lv_memset(&uv_info, 0, sizeof(uv_info));
    uv_info.loop = loop;
    uv_info.disp = result->disp;
    uv_info.indev = result->indev;
#ifdef CONFIG_UINPUT_TOUCH
    uv_info.uindev = result->utouch_indev;
#endif

    data = lv_nuttx_uv_init(&uv_info);
    uv_run(loop, UV_RUN_DEFAULT);
    lv_nuttx_uv_deinit(&data);
}

// Timer callback function that is executed repeatedly during key presses
static void press_timer_cb(lv_timer_t *timer)
{
    // Locking to protect global variables
    pthread_mutex_lock(&asr_result_mutex);
    const char* asr_result_text = global_asr_result.result;
    pthread_mutex_unlock(&asr_result_mutex);

    if (asr_result_text != NULL) {
        printf("ASR result text: %s, length: %zu\n", asr_result_text, strlen(asr_result_text));
        fflush(stdout);
        lv_textarea_set_text(g_audio_text.ui.textarea, asr_result_text);
        printf("lv_textarea_set_text\n");
    } else {
        lv_textarea_set_text(g_audio_text.ui.textarea, "No ASR result available");
    }
    
    aitool_cmd_start_exec(&aitool, 0);
}

static void key_press_cb(lv_event_t* e)
{
    lv_obj_t* obj = lv_event_get_target(e);
    if (obj == NULL) {
        return;
    }

    // Start timer, every 30ms
    if (press_timer == NULL) {
        press_timer = lv_timer_create(press_timer_cb, 30, NULL);
    }
}

static void key_release_cb(lv_event_t* e)
{
    lv_obj_t* obj = lv_event_get_target(e);
    if (obj == NULL) {
        return;
    }

    // Locking to protect global variables
    pthread_mutex_lock(&asr_result_mutex);
    const char* asr_result_text = global_asr_result.result;
    pthread_mutex_unlock(&asr_result_mutex);

    // lv_textarea_set_text(g_audio_text.ui.textarea, "Key Release");
    lv_textarea_set_text(g_audio_text.ui.textarea, asr_result_text);
    int ret = aitool_cmd_finish_exec(&aitool, 0);
    // int ret2 = aitool_cmd_close_exec(&aitool, 0);

    if(press_timer != NULL){
        lv_timer_del(press_timer);
        press_timer = NULL;
    }
    
}

static void clear_text_cb(lv_event_t* e)
{
    lv_obj_t* obj = lv_event_get_target(e);
    if (obj == NULL) {
        return;
    }

    lv_textarea_set_text(g_audio_text.ui.textarea, "");
    int ret = aitool_cmd_finish_exec(&aitool, 0);
}

static void app_create(void)
{
    font = lv_freetype_font_create("/data/res/fonts/MiSans-Normal.ttf", LV_FREETYPE_FONT_RENDER_MODE_BITMAP, 24, LV_FREETYPE_FONT_STYLE_NORMAL);
    // Create main container
    g_audio_text.ui.root = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(g_audio_text.ui.root);
    lv_obj_center(g_audio_text.ui.root);
    lv_obj_set_size(g_audio_text.ui.root, DEMO_WIDTH, DEMO_HEIGHT);
    lv_obj_set_flex_flow(g_audio_text.ui.root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_audio_text.ui.root, LV_FLEX_ALIGN_CENTER,\
                     LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollbar_mode(g_audio_text.ui.root, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(g_audio_text.ui.root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(g_audio_text.ui.root, 16, 0); // Add padding
    
    // Create title
    g_audio_text.ui.title = lv_label_create(g_audio_text.ui.root);
    lv_label_set_text(g_audio_text.ui.title, "Speech to Text");
    lv_obj_set_style_text_color(g_audio_text.ui.title, lv_color_hex(0x2C3E50), 0);
    lv_obj_set_style_text_font(g_audio_text.ui.title, &lv_font_montserrat_32, 0);
    lv_obj_set_size(g_audio_text.ui.title, LV_PCT(100), LV_PCT(10));
    lv_obj_set_style_text_align(g_audio_text.ui.title, LV_TEXT_ALIGN_CENTER, 0);
    
    // Create text area
    g_audio_text.ui.textarea = lv_textarea_create(g_audio_text.ui.root);
    lv_obj_set_size(g_audio_text.ui.textarea, LV_PCT(100), LV_PCT(50));
    lv_obj_set_style_text_align(g_audio_text.ui.textarea, LV_TEXT_ALIGN_CENTER, 0);
    // lv_obj_set_style_border_opa(g_audio_text.ui.textarea, LV_OPA_TRANSP, 0); 
    // lv_obj_set_style_bg_opa(g_audio_text.ui.textarea, LV_OPA_TRANSP, 0); 
    lv_obj_set_style_border_color(g_audio_text.ui.textarea, lv_color_hex(0xBDC3C7), 0);
    lv_obj_set_style_border_width(g_audio_text.ui.textarea, 1, 0);
    lv_obj_set_style_radius(g_audio_text.ui.textarea, 8, 0);
    lv_obj_set_style_bg_color(g_audio_text.ui.textarea, lv_color_hex(0xF8FAFC), 0);
    lv_obj_set_style_text_font(g_audio_text.ui.title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_font(g_audio_text.ui.textarea, font, 0);
    lv_textarea_set_placeholder_text(g_audio_text.ui.textarea, "Speech recognition results will appear here...");

    // Create voice button
    g_audio_text.ui.voice_btntnm = lv_button_create(g_audio_text.ui.root);
    lv_obj_set_size(g_audio_text.ui.voice_btntnm, LV_PCT(20), LV_PCT(20));
    
    lv_obj_set_style_radius(g_audio_text.ui.voice_btntnm, LV_RADIUS_CIRCLE, 0); // Circular button

    lv_obj_set_style_bg_color(g_audio_text.ui.voice_btntnm, lv_color_hex(0x3498DB), 0); // Blue background
    lv_obj_set_style_bg_color(g_audio_text.ui.voice_btntnm, lv_color_hex(0x2980B9), LV_STATE_PRESSED); // Pressed state color
    
    g_audio_text.ui.clear_btnm = lv_btn_create(g_audio_text.ui.root);
    lv_obj_set_size(g_audio_text.ui.clear_btnm, 40, 40);
    lv_obj_set_style_bg_opa(g_audio_text.ui.clear_btnm, LV_OPA_0, 0); 
    lv_obj_set_style_border_opa(g_audio_text.ui.clear_btnm, LV_OPA_0, 0);

    lv_obj_t * clear_icon = lv_label_create(g_audio_text.ui.clear_btnm);
    lv_label_set_text(clear_icon, LV_SYMBOL_CLOSE); 
    lv_obj_set_style_text_color(clear_icon, lv_color_hex(0x666666), 0);
    lv_obj_center(clear_icon);

    // mic imag
    mic_obj = lv_image_create(g_audio_text.ui.voice_btntnm);
    lv_img_set_src(mic_obj, &mic);
    lv_img_set_zoom(mic_obj,128);
    lv_obj_align(mic_obj, LV_ALIGN_CENTER, 0, 0); 
    // Add event callbacks
    lv_obj_add_event_cb(g_audio_text.ui.voice_btntnm, key_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_audio_text.ui.voice_btntnm, key_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(g_audio_text.ui.clear_btnm, clear_text_cb, LV_EVENT_CLICKED, NULL); 
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/
int main(int argc, FAR char* argv[])
{
    // aitool
    pthread_attr_t attr;
    char* buffer = NULL;
    pthread_t thread;
    size_t len = 0;
    ssize_t n;
    int ret;

    memset(&aitool, 0, sizeof(aitool));

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, CONFIG_AI_TOOL_STACKSIZE);
    ret = pthread_create(&thread, &attr, aitool_uvloop_thread, &aitool);
    if (ret < 0)
        goto out;

    usleep(1000); /* let uvloop run. */
    int num = aitool_cmd_create_asr_engine_exec(&aitool);

    // init lvgl
    lv_nuttx_dsc_t info;
    lv_nuttx_result_t result;
    uv_loop_t ui_loop;

    
    lv_memset(&ui_loop, 0, sizeof(uv_loop_t));

    if (lv_is_initialized()) {
        LV_LOG_ERROR("LVGL already initialized! aborting.");
        return -1;
    }

    lv_init();

    lv_nuttx_dsc_init(&info);
    lv_nuttx_init(&info, &result);

    if (result.disp == NULL) {
        LV_LOG_ERROR("lv_demos initialization failure!");
        return 1;
    }

    app_create();

    // refresh lvgl ui
    lv_nuttx_uv_loop(&ui_loop, &result);

    lv_nuttx_deinit(&result);
    lv_deinit();

out:
    pthread_join(thread, NULL);
    return 0;
}