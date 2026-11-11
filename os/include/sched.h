#ifndef JULYOS_SCHED_H
#define JULYOS_SCHED_H

#include "types.h"

#define MAX_TASKS      16
#define TASK_NAME_LEN  32

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY  = 1,
    TASK_RUNNING = 2,
    TASK_DONE   = 3
} task_state_t;

typedef struct {
    uint32_t     tid;
    char         name[TASK_NAME_LEN];
    task_state_t state;
    uint64_t     ticks;
    uint64_t     work_progress; /* 0..100 visual gauge */
    uint64_t     rsp;
    uint64_t     stack_base;
} task_info_t;

typedef void (*task_entry_t)(void *arg);

void     sched_init(void);
int      sched_spawn(const char *name, task_entry_t entry, void *arg);
void     sched_yield(void);
uint64_t sched_switch_count(void);
size_t   sched_snapshot(task_info_t *out, size_t max_out);
task_info_t *sched_current(void);

#endif /* JULYOS_SCHED_H */

