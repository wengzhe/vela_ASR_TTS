
#include <ai_asr.h>
#include <ai_tts.h>
#include <ai_conversation.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <uv.h>
#include <uv_async_queue.h>

#include "ai_asr_internal.h"

#define AITOOL_MAX_CHAIN 16
#define AITOOL_MAX_ARGC 16
#define AITOOL_ASR 1
#define AITOOL_TTS 2

typedef struct aitool_chain_s {
    int id;
    void* handle;
    void* extra;
    int handle_type;
} aitool_chain_t;

typedef struct aitool_s {
    aitool_chain_t chain[AITOOL_MAX_CHAIN];
    uv_loop_t loop;
    uv_async_queue_t asyncq;
    uv_timer_t timer;
    int64_t asr_start_time;
    int64_t asr_cost;
    int64_t asr_first_work_cost;
} aitool_t;

typedef int (*aitool_func)(aitool_t* aitool, int argc, char** argv);

typedef struct aitool_cmd_s {
    const char* cmd; /* The command text */
    aitool_func pfunc; /* Pointer to command handler */
    const char* help; /* The help text */
} aitool_cmd_t;

typedef char* string_t;
// 定义全局变量
extern asr_result_t global_asr_result;
extern pthread_mutex_t asr_result_mutex;

int aitool_cmd_create_asr_engine_exec(aitool_t*);
int aitool_cmd_start_exec(aitool_t*, int);
int aitool_cmd_finish_exec(aitool_t*, int);
int aitool_cmd_close_exec(aitool_t*, int);
void* aitool_uvloop_thread(void* arg);