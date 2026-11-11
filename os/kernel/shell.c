#include "kernel_abi.h"
#include "kmalloc.h"
#include "kprintf.h"
#include "ramfs.h"
#include "sched.h"
#include "gfx.h"
#include "string.h"

static void run_cmd(const char *cmd) {
    if (strcmp(cmd, "help") == 0) {
        kprintf("Commands: help, mem, ps, ls, uptime, halt\n");
    } else if (strcmp(cmd, "mem") == 0) {
        kmalloc_stats_t st;
        kmalloc_get_stats(&st);
        kprintf("PMM: free=%llu KB / total=%llu KB | Heap used=%llu / %llu B\n",
                pmm_free_bytes() / 1024ULL, pmm_total_bytes() / 1024ULL,
                st.used_heap_bytes, st.total_heap_bytes);
    } else if (strcmp(cmd, "ps") == 0) {
        task_info_t snap[MAX_TASKS];
        size_t n = sched_snapshot(snap, MAX_TASKS);
        for (size_t i = 0; i < n; i++) {
            kprintf("  TID %u  %-14s state=%u ticks=%llu prog=%llu%%\n",
                    snap[i].tid, snap[i].name, (unsigned)snap[i].state,
                    snap[i].ticks, snap[i].work_progress);
        }
    } else if (strcmp(cmd, "ls") == 0) {
        ramfs_dirent_t ents[RAMFS_MAX_FILES];
        size_t n = ramfs_list(ents, RAMFS_MAX_FILES);
        for (size_t i = 0; i < n; i++) {
            kprintf("  %-18s (%llu bytes)\n", ents[i].name, ents[i].size);
        }
    } else if (strcmp(cmd, "uptime") == 0) {
        char buf[64];
        if (ramfs_read("/proc/uptime", buf, sizeof(buf)) > 0) kprintf("%s", buf);
    } else if (strcmp(cmd, "halt") == 0) {
        kprintf("[JULYOS] ACPI shutdown requested.\n");
        arch_qemu_exit(0);
    } else if (cmd[0] != '\0') {
        kprintf("Unknown command: %s\n", cmd);
    }
}

void shell_poll(void) {
    static char cmd_buf[64];
    static size_t cmd_len = 0;

    int ch;
    while ((ch = serial_try_getc()) >= 0) {
        if (ch == '\r' || ch == '\n') {
            serial_putc('\n');
            cmd_buf[cmd_len] = '\0';
            run_cmd(cmd_buf);
            cmd_len = 0;
            kprintf("julyos> ");
        } else if (ch == 0x7F || ch == '\b') {
            if (cmd_len > 0) cmd_len--;
        } else if (cmd_len + 1 < sizeof(cmd_buf)) {
            cmd_buf[cmd_len++] = (char)ch;
            serial_putc((char)ch);
        }
    }
}

