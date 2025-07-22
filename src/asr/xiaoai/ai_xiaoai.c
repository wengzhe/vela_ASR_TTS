/****************************************************************************
 * frameworks/ai/src/asr/xiaoai/ai_xiaoai.c
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

#if defined(CONFIG_AI_XIAOAI_ENGINE) && CONFIG_AI_XIAOAI_ENGINE

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>

#include "ai_common.h"
#include "ai_voice_plugin.h"
#include "data_proxy.h"
#include "include/wear.pb.h"
#include "miwear_media_session.h"
#include "wear_aivs.pb.h"

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct xiaoai_context {
    void* subscriber; // subsciber_t*
    voice_callback_t cb;
    void* cookie;
    voice_env_params_t* env_params;
    int session_id;
} xiaoai_context_t;

typedef struct {
    int pb_code;
    voice_error_t voice_code;
    char const* str_code;
} voice_err_code_t;

static voice_err_code_t errcode_map[] = {
    { ResultType_ERROR_NETWORK, voice_error_network,
        "ResultType_ERROR_NETWORK" },
    { ResultType_ERROR_AUTH, voice_error_auth, "ResultType_ERROR_AUTH" },
    { ResultType_ERROR_TOO_MANY_DEVICES, voice_error_too_many_devices,
        "ResultType_ERROR_TOO_MANY_DEVICES" },
    { ResultType_ERROR_CONTENT_TOO_LONG, voice_error_content_too_long,
        "ResultType_ERROR_CONTENT_TOO_LONG" },
    { ResultType_AIVS_ERROR_ASR_TIMEOUT, voice_error_asr_timeout,
        "ResultType_AIVS_ERROR_ASR_TIMEOUT" },
    { ResultType_SYSTEM_TRUNCATIONNOTIFICATION, voice_error_listen_timeout,
        "ResultType_SYSTEM_TRUNCATIONNOTIFICATION" },
    { ResultType_AIVS_ERROR_TTS_TIMEOUT, voice_error_tts_timeout,
        "ResultType_AIVS_ERROR_TTS_TIMEOUT" },

    { 0, 0, NULL },
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int xiaoai_exit(void* engine)
{
    if (engine == NULL)
        return -EINVAL;

    BTMsgPacket voice_packet;
    memset(&voice_packet, 0, sizeof(BTMsgPacket));
    voice_packet.msg_id = BTMsgPacket_MsgID_VOICE_CMD;
    voice_packet.which_payload = BTMsgPacket_voice_cmd_tag;
    VoiceCmd* voice_cmd = &voice_packet.payload.voice_cmd;
    voice_cmd->cmd_id = VoiceCmd_CmdID_EXIT;
    return data_proxy_internal_pb_send(&voice_packet, NULL, NULL);
}

static voice_error_t get_errcode(int code)
{
    voice_err_code_t* err_code = NULL;

    for (int i = 0; (!err_code || err_code->str_code != NULL); i++) {
        err_code = &errcode_map[i];
        if (err_code->pb_code == code)
            return err_code->voice_code;
    }

    return voice_error_success;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void xiaoai_ai_ins_data_callback(mq_app_data_t* mq_app_data, void* arg)
{
    mq_app_data_t* mq_data = mq_app_data;
    xiaoai_context_t* ctx = (xiaoai_context_t*)arg;

    AI_INFO("ResultType_cb\n");

    if (mq_data->data_len == 0) {
        return;
    }

    WearPacket* pack = (WearPacket*)mq_data->data;
    if (pack->type != WearPacket_Type_AIVS
        || pack->id != Aivs_AivsID_SYNC_INSTRUCTION_LIST) {
        return;
    }

    Aivs* aivs = &pack->payload.aivs;

    for (int i = 0; i < aivs->payload.instruction_list.list_count; i++) {
        AivsInstruction* list = &aivs->payload.instruction_list.list[i];

        AI_INFO("ResultType_cb_type = %d\n", list->result_type);

        switch (list->result_type) {
        case ResultType_START_LISTENING: {
            break;
        }

        case ResultType_END_LISTENING:
        case ResultType_STOP_CAPTURE: {
            AI_INFO("ResultType_END_LISTENING = %d\n", list->result_type);
            // todo: stop capture
            if (ctx->cb)
                ctx->cb(voice_event_complete, NULL, ctx->cookie);
            break;
        }

        case ResultType_SYSTEM_TRUNCATIONNOTIFICATION: {
            // https://developers.xiaoai.mi.com/documents/Home?type=/api/doc/render_markdown/VoiceserviceAccess/Device/develop/SDKDocument/AndroidInstruction#71-aivsconfig
            AI_INFO("ResultType_END_LISTENING = %d\n", list->result_type);
            // todo: stop capture
            voice_result_t result = { 0 };
            result.error_code = get_errcode(list->result_type);
            if (ctx->cb)
                ctx->cb(voice_event_error, &result, ctx->cookie);
            break;
        }

        case ResultType_DIALOG_FINISH: {
            break;
        }

        // ASR
        case ResultType_RECOGNIZE_RESULT: {
            AI_INFO("ResultType_RECOGNIZE_RESULT\n");
            SpeechRecognizeResult* p_recognize_result = &(list->recognize_result);
            for (int idx = 0; idx < p_recognize_result->results.list_count; idx++) {
                SpeechRecognizeResultItem p_item = p_recognize_result->results.list[idx];
                AI_INFO("ASR Result:%s\n", p_item.text);
                voice_result_t result = { 0 };
                result.result = p_item.text;
                if (ctx->cb)
                    ctx->cb(voice_event_result, &result, ctx->cookie);
            }
            if (p_recognize_result->is_final) {
                // todo: stop capture
                if (ctx->cb)
                    ctx->cb(voice_event_complete, NULL, ctx->cookie);
            }
            break;
        }

        case ResultType_ERROR_NETWORK:
        case ResultType_ERROR_AUTH:
        case ResultType_ERROR_TOO_MANY_DEVICES:
        case ResultType_ERROR_CONTENT_TOO_LONG:
        case ResultType_AIVS_ERROR_ASR_TIMEOUT:
        case ResultType_AIVS_ERROR_TTS_TIMEOUT: {
            AI_INFO("ResultType_ERROR = %d\n", list->result_type);
            voice_result_t result = { 0 };
            result.error_code = get_errcode(list->result_type);
            if (ctx->cb)
                ctx->cb(voice_event_error, &result, ctx->cookie);
            break;
        }

        case ResultType_TEMPLATE_TOAST: {
            // TTS
            AI_INFO("ResultType_TEMPLATE_TOAST\n");
            break;
        }

        case ResultType_TEMPLATE_GENERAL:
        case ResultType_TEMPLATE_GENERAL2:
        case ResultType_TEMPLATE_WEATHER:
        case ResultType_TEMPLATE_LISTS:
        case ResultType_TEMPLATE_SWITCHPANEL:
        case ResultType_TEMPLATE_SWITCHPANELLIST:
        case ResultType_TEMPLATE_DEVICELIST:
        case ResultType_ALERTS_SETALERT:
        case ResultType_ALERTS_DELIVERALERTINTENTION:
        case ResultType_ALERTS_STOPALERT: {
            // NLP
            AI_INFO("ResultType_TEMPLATE\n");
            break;
        }

        case ResultType_SPEECHSYNTHESIZER_SPEAK: {
            // TTS
            AI_INFO("ResultType_SPEECHSYNTHESIZER_SPEAK\n");
            break;
        }

        case ResultType_SPEECHRECOGNIZER_EXPECTSPEECH: {
            AI_INFO("ResultType_SPEECHRECOGNIZER_EXPECTSPEECH\n");
            // todo: ASR need input extra audio data
            break;
        }

        case ResultType_LAUNCHER_LAUNCHAPP:
        case ResultType_BRIGHTNESSCONTROLLER_ADJUSTBRIGHTNESS:
        case ResultType_SYSTEM_SETPROPERTY:
        case ResultType_APPLICATION_OPERATE:
        case ResultType_PLAYBACKCONTROLLER:
        case ResultType_PLAYBACKCONTROLLER_STOP:
        case ResultType_PLAYBACKCONTROLLER_PLAY:
        case ResultType_PLAYBACKCONTROLLER_PAUSE:
        case ResultType_PLAYBACKCONTROLLER_NEXT:
        case ResultType_PLAYBACKCONTROLLER_PREV:
        case ResultType_PLAYBACKCONTROLLER_CONTINUEPLAYING:
        case ResultType_SPEAKER_SETMUTE:
        case ResultType_SPEAKER_SETVOLUME:
        case ResultType_SPEAKER_AJUSTVOLUME:
        case ResultType_EXECUTE_DEVICESKILL:
        case ResultType_TEMPLATE_PLAYINFO:
        case ResultType_MAKE_CALL:
        case ResultType_WEARABLE_CONTROLLER_SWITCH:
        case ResultType_WEARABLE_CONTROLLER_EXECUTE:
        case ResultType_LLM_LOADING_CARD:
        case ResultType_LLM_DECLARATION_CONTENT:
        case ResultType_LLM_ILLEGAL_CONTENT:
        case ResultType_SHOW_CONTACTS: {
            // NLP
            AI_INFO("ResultType_NLP = %d\n", list->result_type);
            break;
        }
        case ResultType_LLM_FINISH_STREAM:
        case ResultType_LLM_TOAST_STREAM: {
            // NLP
            break;
        }

        default:
            break;
        }
    }
}

static int xiaoai_init(void* engine, const voice_init_params_t* param)
{
    xiaoai_context_t* ctx = (xiaoai_context_t*)engine;

    if (engine == NULL || param == NULL)
        return -EINVAL;

    ctx->subscriber = data_proxy_wear_pb_subscribe(WearPacket_Type_AIVS, Aivs_AivsID_SYNC_INSTRUCTION_LIST, xiaoai_ai_ins_data_callback, ctx);
    AI_INFO("asr_xiaoai_init:%p", media_session_loop());
    return 0;
}

static int xiaoai_uninit(void* engine)
{
    xiaoai_context_t* ctx = (xiaoai_context_t*)engine;
    int ret;

    if (engine == NULL)
        return -EINVAL;

    // wwc test
    ret = xiaoai_exit(engine);

    if (ctx->subscriber != NULL) {
        data_proxy_wear_pb_unsubscribe(ctx->subscriber);
        ctx->subscriber = NULL;
    }

    if (ctx->env_params) {
        free(ctx->env_params);
        ctx->env_params = NULL;
    }

    free(engine);

    return ret;
}

static int xiaoai_event_cb(void* engine, voice_callback_t callback, void* cookie)
{
    xiaoai_context_t* ctx = (xiaoai_context_t*)engine;

    if (engine == NULL)
        return -EINVAL;

    ctx->cb = callback;
    ctx->cookie = cookie;
    return 0;
}

static int xiaoai_start(void* engine, const voice_audio_info_t* audio_info)
{
    xiaoai_context_t* ctx = (xiaoai_context_t*)engine;
    BTMsgPacket voice_packet;

    if (engine == NULL)
        return -EINVAL;

    memset(&voice_packet, 0, sizeof(BTMsgPacket));
    voice_packet.msg_id = BTMsgPacket_MsgID_VOICE_CMD;
    voice_packet.which_payload = BTMsgPacket_voice_cmd_tag;
    VoiceCmd* voice_cmd = &voice_packet.payload.voice_cmd;
    voice_cmd->cmd_id = VoiceCmd_CmdID_START;
    voice_cmd->which_payload = VoiceCmd_start_tag;
    voice_cmd->payload.start.session_id = ctx->session_id;
    if (false) {
        voice_cmd->payload.start.has_tts_enable = true;
        voice_cmd->payload.start.tts_enable = 1;
    }

    ctx->session_id += 1;

    return data_proxy_internal_pb_send(&voice_packet, NULL, NULL);
}

static int xiaoai_write_audio(void* engine, const char* data, int len)
{
    int ret;

    if (engine == NULL || data == NULL || len <= 0)
        return -EINVAL;

    BTMsgPacket voice_packet;
    memset(&voice_packet, 0, sizeof(BTMsgPacket));
    voice_packet.msg_id = BTMsgPacket_MsgID_VOICE_CMD;
    voice_packet.which_payload = BTMsgPacket_voice_cmd_tag;
    VoiceCmd* voice_cmd = &voice_packet.payload.voice_cmd;
    voice_cmd->cmd_id = VoiceCmd_CmdID_DATA;
    voice_cmd->which_payload = VoiceCmd_data_tag;
    VoiceCmd_Data* voice_data = &voice_cmd->payload.data;
    voice_data->data
        = (pb_bytes_array_t*)malloc(sizeof(pb_bytes_array_t) + len);
    if (voice_data->data == NULL) {
        return -ENOMEM;
    }
    voice_data->data->size = len;
    memcpy(voice_data->data->bytes, data, len);
    ret = data_proxy_internal_pb_send(&voice_packet, NULL, NULL);
    free(voice_data->data);

    return ret;
}

static int xiaoai_finish(void* engine)
{
    if (engine == NULL)
        return -EINVAL;

    BTMsgPacket voice_packet;
    memset(&voice_packet, 0, sizeof(BTMsgPacket));
    voice_packet.msg_id = BTMsgPacket_MsgID_VOICE_CMD;
    voice_packet.which_payload = BTMsgPacket_voice_cmd_tag;
    VoiceCmd* voice_cmd = &voice_packet.payload.voice_cmd;
    voice_cmd->cmd_id = VoiceCmd_CmdID_DATA_END;
    return data_proxy_internal_pb_send(&voice_packet, NULL, NULL);
}

static int xiaoai_cancel(void* engine)
{
    if (engine == NULL)
        return -EINVAL;

    return 0;
}

static voice_env_params_t* xiaoai_get_env_params(void* engine)
{
    xiaoai_context_t* ctx = (xiaoai_context_t*)engine;
    voice_env_params_t* env_params;

    if (engine == NULL)
        return NULL;

    if (ctx->env_params) {
        AI_INFO("xiaoai_get_env_params");
        return ctx->env_params;
    }

    env_params = (voice_env_params_t*)malloc(sizeof(voice_env_params_t));
    env_params->format = "format=opusraw:sample_rate=16000:ch_layout=mono:"
                         "b=32000:vbr=0:compression_level=1";
    env_params->force_format = 0;
    env_params->loop = media_session_loop();
    env_params->asyncq = NULL;
    ctx->env_params = env_params;

    AI_INFO("xiaoai_get_env_params");

    return env_params;
}

voice_plugin_t xiaoai_asr_engine_plugin = {
    .name = "xiaoai_asr",
    .priv_size = sizeof(xiaoai_context_t),
    .init = xiaoai_init,
    .uninit = xiaoai_uninit,
    .event_cb = xiaoai_event_cb,
    .start = xiaoai_start,
    .write_audio = xiaoai_write_audio,
    .finish = xiaoai_finish,
    .cancel = xiaoai_cancel,
    .get_env = xiaoai_get_env_params,
};

#endif /* CONFIG_AI_XIAOAI_ENGINE */
