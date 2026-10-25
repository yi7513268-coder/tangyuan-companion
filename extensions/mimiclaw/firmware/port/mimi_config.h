#pragma once
/* Deliberately not the upstream general-GPIO/Telegram firmware configuration. */
#define MIMI_AGENT_STACK (24 * 1024)
#define MIMI_AGENT_PRIO 5
#define MIMI_AGENT_CORE 1
#define MIMI_AGENT_MAX_TOOL_ITER 12
#define MIMI_MAX_TOOL_CALLS 4
#define MIMI_AGENT_MAX_HISTORY 4
#define MIMI_CONTEXT_BUF_SIZE (16 * 1024)
#define MIMI_LLM_STREAM_BUF_SIZE (32 * 1024)
#define MIMI_AGENT_SEND_WORKING_STATUS 0
#define MIMI_BUS_QUEUE_LEN 2
