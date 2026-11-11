#include "sched.h"
#include "kernel_abi.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "string.h"

#define TASK_STACK_PAGES 2 /* 8 KiB per kernel thread */

static task_info_t tasks[MAX_TASKS];
static int current_idx = 0;
static uint64_t total_switches = 0;
static uint32_t next_tid = 1;
static bool sched_active = false;

static void task_trampoline_exit(void) {
    arch_cli();
    tasks[current_idx].state = TASK_DONE;
    arch_sti();
    while (1) {
        sched_yield();
        arch_hlt();
    }
}

void sched_init(void) {
    memset(tasks, 0, sizeof(tasks));
    current_idx = 0;
    total_switches = 0;
    next_tid = 1;

    /* Slot 0 represents the bootstrap kernel main/dashboard thread */
    tasks[0].tid = next_tid++;
    strcpy(tasks[0].name, "kmain_gui");
    tasks[0].state = TASK_RUNNING;
    tasks[0].ticks = 0;
    tasks[0].work_progress = 100;
    sched_active = true;
}

int sched_spawn(const char *name, task_entry_t entry, void *arg) {
    arch_cli();
    int slot = -1;
    for (int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_UNUSED) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        arch_sti();
        return -1;
    }

    uint8_t *stack_mem = (uint8_t *)kmalloc(TASK_STACK_PAGES * PAGE_SIZE);
    if (!stack_mem) {
        arch_sti();
        return -1;
    }
    memset(stack_mem, 0, TASK_STACK_PAGES * PAGE_SIZE);

    /* Stack grows down; align top to 16 bytes and push return address trampoline */
    uint64_t stack_top = ((uint64_t)(uintptr_t)stack_mem + TASK_STACK_PAGES * PAGE_SIZE) & ~0xFULL;
    stack_top -= sizeof(uint64_t);
    *(uint64_t *)(uintptr_t)stack_top = (uint64_t)(uintptr_t)task_trampoline_exit;

    /* Synthesize initial interrupt_frame_t matching arch/x86_64/isr.S */
    stack_top -= sizeof(interrupt_frame_t);
    interrupt_frame_t *frame = (interrupt_frame_t *)(uintptr_t)stack_top;
    memset(frame, 0, sizeof(*frame));
    frame->rdi    = (uint64_t)(uintptr_t)arg;
    frame->rip    = (uint64_t)(uintptr_t)entry;
    frame->cs     = 0x08ULL;
    frame->rflags = 0x202ULL; /* IF enabled */
    frame->rsp    = stack_top + sizeof(interrupt_frame_t);
    frame->ss     = 0x10ULL;

    tasks[slot].tid = next_tid++;
    strncpy(tasks[slot].name, name ? name : "worker", TASK_NAME_LEN - 1);
    tasks[slot].name[TASK_NAME_LEN - 1] = '\0';
    tasks[slot].state = TASK_READY;
    tasks[slot].ticks = 0;
    tasks[slot].work_progress = 0;
    tasks[slot].rsp = stack_top;
    tasks[slot].stack_base = (uint64_t)(uintptr_t)stack_mem;

    arch_sti();
    return (int)tasks[slot].tid;
}

void sched_yield(void) {
    __asm__ volatile ("int $0x80");
}

uint64_t sched_on_tick(uint64_t current_rsp) {
    if (!sched_active) return current_rsp;

    tasks[current_idx].rsp = current_rsp;
    tasks[current_idx].ticks++;
    if (tasks[current_idx].state == TASK_RUNNING) {
        tasks[current_idx].state = TASK_READY;
    }

    int next = current_idx;
    for (int i = 1; i <= MAX_TASKS; i++) {
        int probe = (current_idx + i) % MAX_TASKS;
        if (tasks[probe].state == TASK_READY) {
            next = probe;
            break;
        }
    }

    if (next != current_idx) {
        total_switches++;
    }
    current_idx = next;
    tasks[current_idx].state = TASK_RUNNING;
    return tasks[current_idx].rsp;
}

uint64_t sched_switch_count(void) {
    return total_switches;
}

size_t sched_snapshot(task_info_t *out, size_t max_out) {
    size_t count = 0;
    for (int i = 0; i < MAX_TASKS && count < max_out; i++) {
        if (tasks[i].state != TASK_UNUSED) {
            out[count++] = tasks[i];
        }
    }
    return count;
}

task_info_t *sched_current(void) {
    return &tasks[current_idx];
}

