/****************************************************************************
 * framework/ai/ai_tool.c
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

#include <ai_asr.h>
#include <ai_tts.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <uv.h>
#include <uv_async_queue.h>

#include "ai_asr_internal.h"
#include "ai_tool.h"


/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/
asr_result_t global_asr_result = {0};
pthread_mutex_t asr_result_mutex = PTHREAD_MUTEX_INITIALIZER; // 定义互斥锁
#define AITOOL_MAX_CHAIN 16
#define AITOOL_MAX_ARGC 16
#define AITOOL_ASR 1
#define AITOOL_TTS 2

#define GET_ARG_FUNC(out_type, arg)                  \
    static out_type get_##out_type##_arg(char* arg); \
    static out_type get_##out_type##_arg(char* arg)

#define GET_ARG(out_type, arg) \
    get_##out_type##_arg(arg)

#define CMD0(func)                                                        \
    int aitool_cmd_##func##_exec(aitool_t* aitool);                \
    static int aitool_cmd_##func(aitool_t* aitool, int argc, char** argv) \
    {                                                                     \
        (void)argc;                                                       \
        (void)argv;                                                       \
        return aitool_cmd_##func##_exec(aitool);                          \
    }                                                                     \
    int aitool_cmd_##func##_exec(aitool_t* aitool)

#define CMD1(func, type1, arg1)                                           \
    int aitool_cmd_##func##_exec(aitool_t* aitool, type1 arg1);    \
    static int aitool_cmd_##func(aitool_t* aitool, int argc, char** argv) \
    {                                                                     \
        type1 arg1;                                                       \
        arg1 = (argc > 1) ? GET_ARG(type1, argv[1]) : 0;                  \
        return aitool_cmd_##func##_exec(aitool, arg1);                    \
    }                                                                     \
    int aitool_cmd_##func##_exec(aitool_t* aitool, type1 arg1)

#define CMD2(func, type1, arg1, type2, arg2)                                       \
    int aitool_cmd_##func##_exec(aitool_t* aitool, type1 arg1, type2 arg2); \
    static int aitool_cmd_##func(aitool_t* aitool, int argc, char** argv)          \
    {                                                                              \
        type1 arg1;                                                                \
        type2 arg2;                                                                \
        arg1 = (argc > 1) ? GET_ARG(type1, argv[1]) : 0;                           \
        arg2 = (argc > 2) ? GET_ARG(type2, argv[2]) : 0;                           \
        return aitool_cmd_##func##_exec(aitool, arg1, arg2);                       \
    }                                                                              \
    int aitool_cmd_##func##_exec(aitool_t* aitool, type1 arg1, type2 arg2)

#define CMD3(func, type1, arg1, type2, arg2, type3, arg3)                                      \
    int aitool_cmd_##func##_exec(aitool_t* aitool, type1 arg1, type2 arg2, type3 arg3); \
    static int aitool_cmd_##func(aitool_t* aitool, int argc, char** argv)                      \
    {                                                                                          \
        type1 arg1;                                                                            \
        type2 arg2;                                                                            \
        type3 arg3;                                                                            \
        arg1 = (argc > 1) ? GET_ARG(type1, argv[1]) : 0;                                       \
        arg2 = (argc > 2) ? GET_ARG(type2, argv[2]) : 0;                                       \
        arg3 = (argc > 3) ? GET_ARG(type3, argv[3]) : 0;                                       \
        return aitool_cmd_##func##_exec(aitool, arg1, arg2, arg3);                             \
    }                                                                                          \
    int aitool_cmd_##func##_exec(aitool_t* aitool, type1 arg1, type2 arg2, type3 arg3)

#define CMD4(func, type1, arg1, type2, arg2, type3, arg3, type4, arg4)                                     \
    int aitool_cmd_##func##_exec(aitool_t* aitool, type1 arg1, type2 arg2, type3 arg3, type4 arg4); \
    static int aitool_cmd_##func(aitool_t* aitool, int argc, char** argv)                                  \
    {                                                                                                      \
        type1 arg1;                                                                                        \
        type2 arg2;                                                                                        \
        type3 arg3;                                                                                        \
        type4 arg4;                                                                                        \
        arg1 = (argc > 1) ? GET_ARG(type1, argv[1]) : 0;                                                   \
        arg2 = (argc > 2) ? GET_ARG(type2, argv[2]) : 0;                                                   \
        arg3 = (argc > 3) ? GET_ARG(type3, argv[3]) : 0;                                                   \
        arg4 = (argc > 4) ? GET_ARG(type4, argv[4]) : 0;                                                   \
        return aitool_cmd_##func##_exec(aitool, arg1, arg2, arg3, arg4);                                   \
    }                                                                                                      \
    int aitool_cmd_##func##_exec(aitool_t* aitool, type1 arg1, type2 arg2, type3 arg3, type4 arg4)


/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/****************************************************************************
 * Private Function
 ****************************************************************************/

GET_ARG_FUNC(int, arg)
{
    return strtol(arg, NULL, 0);
}

GET_ARG_FUNC(string_t, arg)
{
    if (arg && !strlen(arg))
        return NULL;
    return arg;
}

static int64_t aitool_gettime_relative(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void aitool_asr_callback(asr_event_t event, const asr_result_t* result, void* cookie)
{
    aitool_t* aitool = (aitool_t*)cookie;
    int64_t end;

    if (event == asr_event_result) {
        if (aitool->asr_cost == 0) {
            end = aitool_gettime_relative();
            aitool->asr_cost = end - aitool->asr_start_time;
            printf("Asr result fisrt cost: %lld\n", aitool->asr_cost);
        }
        if (aitool->asr_first_work_cost == 0 && result->result && strlen(result->result) > 0) {
            end = aitool_gettime_relative();
            aitool->asr_first_work_cost = end - aitool->asr_start_time;
            pthread_mutex_lock(&asr_result_mutex);
            printf("Asr result fisrt work cost: %lld len:%d\n", aitool->asr_first_work_cost, strlen(result->result));
        }
        // 更新全局变量
        if (global_asr_result.result) {
            free(global_asr_result.result);
        }
        global_asr_result.result = strdup(result->result);
        global_asr_result.duration = result->duration;
        global_asr_result.error_code = result->error_code;
        pthread_mutex_unlock(&asr_result_mutex);
        printf("Asr result: %s\n", global_asr_result.result);
        printf("Asr result: %s\n", result->result);
    } else if (event == asr_event_complete) {
        printf("Asr complete\n");
    } else if (event == asr_event_error) {
        printf("Asr error: %d\n", result->error_code);
    } else if (event == asr_event_start) {
        printf("Asr start\n");
    } else if (event == asr_event_cancel) {
        printf("Asr cancel\n");
    } else if (event == asr_event_closed) {
        printf("Asr closed\n");
    } else {
        printf("Unknown event: %d\n", event);
    }

    printf("Asr aitool:%p\n", aitool);
}

static void aitool_tts_callback(tts_event_t event, const tts_result_t* result, void* cookie)
{
    aitool_t* aitool = (aitool_t*)cookie;

    if (event == tts_event_data) {
        printf("Tts result: %d\n", result->len);
    } else if (event == tts_event_complete) {
        printf("Tts complete\n");
    } else if (event == tts_event_error) {
        printf("Tts error: %d\n", result->error_code);
    } else if (event == tts_event_start) {
        printf("Tts start\n");
    } else if (event == tts_event_stop) {
        printf("Tts cancel\n");
    } else {
        printf("Unknown event: %d\n", event);
    }

    printf("Tts aitool:%p\n", aitool);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

CMD0(create_asr_engine)
{
    asr_init_params_t param;
    int i;

    for (i = 0; i < AITOOL_MAX_CHAIN; i++) {
        if (!aitool->chain[i].handle) {
            param.loop = &aitool->loop;
            param.slience_timeout = 3000;
            aitool->chain[i].handle = ai_asr_create_engine(&param);
            aitool->chain[i].id = i;
            aitool->chain[i].handle_type = AITOOL_ASR;
            break;
        }
    }

    if (i >= AITOOL_MAX_CHAIN || !aitool->chain[i].handle) {
        printf("Create engine failed\n");
        return -1;
    }

    ai_asr_set_listener(aitool->chain[i].handle, aitool_asr_callback, aitool);
    printf("Create engine ID:%d\n", i);

    return i;
}

CMD0(create_tts_engine)
{
    tts_init_params_t param;
    int i;

    for (i = 0; i < AITOOL_MAX_CHAIN; i++) {
        if (!aitool->chain[i].handle) {
            param.loop = &aitool->loop;
            param.engine_type = tts_engine_type_volc;
            aitool->chain[i].handle = ai_tts_create_engine(&param);
            aitool->chain[i].id = i;
            aitool->chain[i].handle_type = AITOOL_TTS;
            break;
        }
    }

    if (i >= AITOOL_MAX_CHAIN || !aitool->chain[i].handle) {
        printf("Create engine failed\n");
        return -1;
    }

    ai_tts_set_listener(aitool->chain[i].handle, aitool_tts_callback, aitool);
    printf("Create engine ID:%d\n", i);

    return 0;
}

CMD1(start, int, id)
{
    asr_handle_t handle;
    int ret;

    printf("Start ID before0:%d\n", id);

    if (id < 0 || id >= AITOOL_MAX_CHAIN)
        return -1;

    handle = aitool->chain[id].handle;
    if (!handle)
        return -1;

    printf("Start ID before:%d\n", id);

    aitool->asr_start_time = aitool_gettime_relative();
    ret = ai_asr_start(handle, NULL);

    printf("Start ID:%d\n", id);

    return ret;
}

CMD2(speak, int, id, string_t, text)
{
    tts_handle_t handle;
    int ret;

    printf("Speak ID before0:%d\n", id);

    if (id < 0 || id >= AITOOL_MAX_CHAIN)
        return -1;

    handle = aitool->chain[id].handle;
    if (!handle)
        return -1;

    printf("Speak ID before:%d\n", id);

    ret = ai_tts_speak(handle, text, NULL);

    printf("Speak ID:%d\n", id);

    return ret;
}

CMD1(finish, int, id)
{
    void* handle;
    int ret;

    if (id < 0 || id >= AITOOL_MAX_CHAIN)
        return -1;

    handle = aitool->chain[id].handle;
    if (!handle)
        return -1;

    if (aitool->chain[id].handle_type == AITOOL_ASR)
        ret = ai_asr_finish(handle);
    else if (aitool->chain[id].handle_type == AITOOL_TTS)
        ret = ai_tts_stop(handle);
    else
        printf("Unknown hanle type!");

    aitool->asr_cost = 0;
    aitool->asr_first_work_cost = 0;

    printf("Finish ID:%d\n", id);

    return ret;
}

CMD1(cancel, int, id)
{
    asr_handle_t handle;
    int ret;

    if (id < 0 || id >= AITOOL_MAX_CHAIN)
        return -1;

    handle = aitool->chain[id].handle;
    if (!handle)
        return -1;

    ret = ai_asr_cancel(handle);

    printf("Cancel ID:%d\n", id);

    return ret;
}

CMD1(is_busy, int, id)
{
    void* handle;
    int ret;

    if (id < 0 || id >= AITOOL_MAX_CHAIN)
        return -1;

    handle = aitool->chain[id].handle;
    if (!handle)
        return -1;

    if (aitool->chain[id].handle_type == AITOOL_ASR)
        ret = ai_asr_is_busy(handle);
    else if (aitool->chain[id].handle_type == AITOOL_TTS)
        ret = ai_tts_is_busy(handle);
    else
        printf("Unknown hanle type!");

    printf("Is_busy ID:%d\n", id);

    return ret;
}

CMD1(close, int, id)
{
    void* handle;
    int ret;

    if (id < 0 || id >= AITOOL_MAX_CHAIN)
        return -1;

    handle = aitool->chain[id].handle;
    if (!handle)
        return -1;

    if (aitool->chain[id].handle_type == AITOOL_ASR) {
        ret = ai_asr_close(handle);
    } else if (aitool->chain[id].handle_type == AITOOL_TTS)
        ret = ai_tts_close(handle);
    else
        printf("Unknown hanle type!");

    if (ret != -1000) {
        aitool->chain[id].handle = NULL;
        aitool->chain[id].id = -1;
        aitool->chain[id].extra = NULL;
        aitool->chain[id].handle_type = 0;
        aitool->asr_cost = 0;
        aitool->asr_first_work_cost = 0;
    } else {
        printf("Close state is an error, please try again!");
    }

    printf("Close ID:%d\n", id);

    return ret;
}

CMD0(quit)
{
    int i;

    for (i = 0; i < AITOOL_MAX_CHAIN; i++) {
        if (aitool->chain[i].handle)
            aitool_cmd_close_exec(aitool, i);
    }

    return 0;
}

static int aitool_cmd_help(const aitool_cmd_t cmds[])
{
    int i;

    for (i = 0; cmds[i].cmd; i++)
        printf("%-16s %s\n", cmds[i].cmd, cmds[i].help);

    return 0;
}

static const aitool_cmd_t g_aitool_cmds[] = {
    { "acreate",
        aitool_cmd_create_asr_engine,
        "Create asr engine (create [UNUSED])" },
    { "tcreate",
        aitool_cmd_create_tts_engine,
        "Create tts engine (create [UNUSED])" },
    { "start",
        aitool_cmd_start,
        "Start engine (start ID)" },
    { "speak",
        aitool_cmd_speak,
        "Speak text (start ID), only for tts" },
    { "finish",
        aitool_cmd_finish,
        "Finish engine (finish ID)" },
    { "cancel",
        aitool_cmd_cancel,
        "Cancel engine (cancel ID)" },
    { "is_busy",
        aitool_cmd_is_busy,
        "Engine is busy (is_busy ID)" },
    { "close",
        aitool_cmd_close,
        "Close engine (close ID)" },
    { "q",
        aitool_cmd_quit,
        "Quit (q)" },
    { "help",
        NULL,
        "Show this message(help)" },

    { 0 },
};

static int aitool_execute(aitool_t* aitool, char* buffer)
{
    char* argv[AITOOL_MAX_ARGC] = { NULL };
    char* saveptr = NULL;
    int ret = 0;
    int argc;
    int x;

    printf("execute cmd:%s\n", buffer);

    argv[0] = strtok_r(buffer, " ", &saveptr);
    for (argc = 1; argc < AITOOL_MAX_ARGC - 1; argc++) {
        argv[argc] = strtok_r(NULL, " ", &saveptr);
        if (argv[argc] == NULL)
            break;
    }

    if (!argv[0])
        return ret;

    printf("execute cmd:%s arg:%d\n", argv[0], argc);

    /* Find the command in our cmd array */

    for (x = 0; g_aitool_cmds[x].cmd; x++) {
        if (!strcmp(argv[0], "help")) {
            aitool_cmd_help(g_aitool_cmds);
            break;
        }

        if (!strcmp(argv[0], g_aitool_cmds[x].cmd)) {
            ret = g_aitool_cmds[x].pfunc(aitool, argc, argv);
            if (ret < 0) {
                printf("cmd %s error %d\n", argv[0], ret);
                ret = 0;
            }

            if (g_aitool_cmds[x].pfunc == aitool_cmd_quit)
                ret = -1;

            break;
        }
    }

    if (g_aitool_cmds[x].cmd == NULL) {
        printf("Unknown cmd: %s\n", argv[0]);
        aitool_cmd_help(g_aitool_cmds);
    }

    return ret;
}

static void volc_uv_handle_close(uv_handle_t* handle, void* arg)
{
    printf("Bye-Bye!\n");
    if (uv_is_active(handle) && !uv_is_closing(handle))
        uv_close(handle, NULL);
}

void aitool_timer_callback(uv_timer_t* handle)
{
    printf("Stopping the timer!\n");
    uv_timer_stop(handle);
    uv_walk(handle->loop, volc_uv_handle_close, NULL);
}

static void aitool_uvasyncq_cb(uv_async_queue_t* asyncq, void* data)
{
    aitool_t* aitool = asyncq->data;
    int ret;

    printf("aitool_execute cmd: %s\n", (char*)data);

    ret = aitool_execute(aitool, data);
    free(data);
    if (ret < 0) {
        printf("Execute cmd error: %d\n", ret);
        uv_handle_set_data((uv_handle_t*)&aitool->asyncq, &aitool->loop);
        uv_timer_init(&aitool->loop, &aitool->timer);
        uv_timer_start(&aitool->timer, aitool_timer_callback, 1000, 0);
    }
}

void* aitool_uvloop_thread(void* arg)
{
    aitool_t* aitool = arg;
    int ret;

    ret = uv_loop_init(&aitool->loop);
    if (ret < 0)
        return NULL;

    aitool->asyncq.data = arg;
    ret = uv_async_queue_init(&aitool->loop, &aitool->asyncq,
        aitool_uvasyncq_cb);
    if (ret < 0)
        goto out;

    printf("[%s][%d] running\n", __func__, __LINE__);
    while (1) {
        ret = uv_run(&aitool->loop, UV_RUN_DEFAULT);
        if (ret == 0)
            break;
    }

out:
    printf("[%s][%d] aitool_running:%d\n", __func__, __LINE__, uv_loop_alive(&aitool->loop));
    uv_stop(&aitool->loop);
    ret = uv_loop_close(&aitool->loop);
    printf("[%s][%d] out:%d\n", __func__, __LINE__, ret);

    return NULL;
}

int main(int argc, char* argv[])
{
    aitool_t aitool;
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
    while (1) {
        printf("aitool> ");
        fflush(stdout);
        n = getline(&buffer, &len, stdin);
        if (n == -1)
            continue;

        if (buffer[n - 1] == '\n') {
            if (n == 1)
                continue;
            else
                buffer[n - 1] = '\0';
        }

        if (buffer[0] == '!') {
#ifdef CONFIG_SYSTEM_SYSTEM
            system(buffer + 1);
#endif
            continue;
        }

        uv_async_queue_send(&aitool.asyncq, buffer);
        if (!strcmp(buffer, "q"))
            break;

        buffer = NULL;
    }

out:
    pthread_join(thread, NULL);
    return 0;
}


