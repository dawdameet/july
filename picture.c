#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <ctype.h>
#include <stdbool.h>

#define MAX_TASKS 32
#define MAX_LOG_LINES 22
#define MAX_LINE_LEN 512

/* ANSI Color Palette */
#define CLR_RESET   "\033[0m"
#define CLR_BOLD    "\033[1m"
#define CLR_DIM     "\033[2m"
#define CLR_CYAN    "\033[1;36m"
#define CLR_BLUE    "\033[1;34m"
#define CLR_GREEN   "\033[1;32m"
#define CLR_YELLOW  "\033[1;33m"
#define CLR_RED     "\033[1;31m"
#define CLR_MAGENTA "\033[1;35m"
#define CLR_WHITE   "\033[1;37m"

static volatile bool g_running = true;

static void handle_sigint(int sig) {
    (void)sig;
    g_running = false;
}

typedef struct {
    char task_id[64];
    char workflow_name[128];
    pid_t pid;
    char command[256];
    bool is_active;
    time_t last_mod;
    char status_str[32];
    char log_lines[MAX_LOG_LINES][MAX_LINE_LEN];
    int log_line_count;
    long rss_kb;
} BlastedTask;

/* Parse command and memory from /proc/<pid> on Linux */
#ifdef __linux__
static void inspect_linux_proc(pid_t pid, char *cmd_out, size_t cmd_sz, long *rss_kb) {
    *cmd_out = '\0';
    *rss_kb = 0;

    char path[128];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        ssize_t n = read(fd, cmd_out, cmd_sz - 1);
        if (n > 0) {
            cmd_out[n] = '\0';
            for (ssize_t i = 0; i < n - 1; i++) {
                if (cmd_out[i] == '\0') cmd_out[i] = ' ';
            }
        }
        close(fd);
    }

    snprintf(path, sizeof(path), "/proc/%d/statm", pid);
    FILE *f = fopen(path, "r");
    if (f) {
        long size, resident;
        if (fscanf(f, "%ld %ld", &size, &resident) == 2) {
            *rss_kb = resident * (sysconf(_SC_PAGESIZE) / 1024);
        }
        fclose(f);
    }
}
#endif

/* Scan /proc to find active child processes running inside /tmp/july_runs */
static void scan_active_july_pids(BlastedTask *tasks, int task_count) {
#ifdef __linux__
    DIR *proc = opendir("/proc");
    if (!proc) return;

    struct dirent *ent;
    while ((ent = readdir(proc)) != NULL) {
        if (!isdigit(ent->d_name[0])) continue;
        pid_t pid = (pid_t)atoi(ent->d_name);

        char cwd_link[64];
        char cwd_target[512];
        snprintf(cwd_link, sizeof(cwd_link), "/proc/%d/cwd", pid);
        ssize_t len = readlink(cwd_link, cwd_target, sizeof(cwd_target) - 1);
        if (len <= 0) continue;
        cwd_target[len] = '\0';

        // Check if CWD is /tmp/july_runs/<task_id>
        char *run_marker = strstr(cwd_target, "/tmp/july_runs/");
        if (run_marker) {
            char *task_id_start = run_marker + strlen("/tmp/july_runs/");
            char task_id_buf[64];
            snprintf(task_id_buf, sizeof(task_id_buf), "%s", task_id_start);
            char *slash = strchr(task_id_buf, '/');
            if (slash) *slash = '\0';

            // Match to task
            for (int i = 0; i < task_count; i++) {
                if (strcmp(tasks[i].task_id, task_id_buf) == 0) {
                    tasks[i].pid = pid;
                    tasks[i].is_active = true;
                    inspect_linux_proc(pid, tasks[i].command, sizeof(tasks[i].command), &tasks[i].rss_kb);
                    break;
                }
            }
        }
    }
    closedir(proc);
#else
    (void)tasks;
    (void)task_count;
#endif
}

/* Read the tail lines of stdout.log */
static void read_tail_log(const char *log_path, BlastedTask *t) {
    t->log_line_count = 0;
    FILE *f = fopen(log_path, "r");
    if (!f) return;

    char buffer[MAX_LINE_LEN];
    char ring_buffer[MAX_LOG_LINES][MAX_LINE_LEN];
    int count = 0;

    while (fgets(buffer, sizeof(buffer), f)) {
        // Strip trailing newline
        size_t l = strlen(buffer);
        while (l > 0 && (buffer[l - 1] == '\n' || buffer[l - 1] == '\r')) {
            buffer[--l] = '\0';
        }
        if (l == 0) continue;

        // Check if manifest workflow title is logged
        if (t->workflow_name[0] == '\0') {
            char *wf_tag = strstr(buffer, "=== July Workflow: ");
            if (wf_tag) {
                char *name_start = wf_tag + strlen("=== July Workflow: ");
                char *paren = strstr(name_start, " (Task ID:");
                if (paren) {
                    size_t wlen = (size_t)(paren - name_start);
                    if (wlen >= sizeof(t->workflow_name)) wlen = sizeof(t->workflow_name) - 1;
                    strncpy(t->workflow_name, name_start, wlen);
                    t->workflow_name[wlen] = '\0';
                }
            }
        }

        snprintf(ring_buffer[count % MAX_LOG_LINES], MAX_LINE_LEN, "%s", buffer);
        count++;
    }
    fclose(f);

    int start = (count > MAX_LOG_LINES) ? (count - MAX_LOG_LINES) : 0;
    int num = (count > MAX_LOG_LINES) ? MAX_LOG_LINES : count;
    for (int i = 0; i < num; i++) {
        snprintf(t->log_lines[i], MAX_LINE_LEN, "%s", ring_buffer[(start + i) % MAX_LOG_LINES]);
    }
    t->log_line_count = num;
}

/* Scan /tmp/july_storage for tasks */
static int scan_july_tasks(BlastedTask *tasks, int max_tasks) {
    DIR *d = opendir("/tmp/july_storage");
    if (!d) return 0;

    int count = 0;
    struct dirent *ent;

    while ((ent = readdir(d)) != NULL && count < max_tasks) {
        if (ent->d_name[0] == '.') continue;

        char log_path[512];
        snprintf(log_path, sizeof(log_path), "/tmp/july_storage/%s/stdout.log", ent->d_name);

        struct stat st;
        if (stat(log_path, &st) == 0) {
            BlastedTask *t = &tasks[count];
            memset(t, 0, sizeof(BlastedTask));
            snprintf(t->task_id, sizeof(t->task_id), "%s", ent->d_name);
            t->last_mod = st.st_mtime;
            snprintf(t->status_str, sizeof(t->status_str), "READY");

            read_tail_log(log_path, t);
            count++;
        }
    }
    closedir(d);

    // Sort by last modified (newest first)
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (tasks[i].last_mod < tasks[j].last_mod) {
                BlastedTask tmp = tasks[i];
                tasks[i] = tasks[j];
                tasks[j] = tmp;
            }
        }
    }

    // Now scan /proc to attach active PIDs
    scan_active_july_pids(tasks, count);

    return count;
}

/* Colorize a log line for terminal screenshots */
static void print_colorized_log_line(const char *line) {
    if (strstr(line, "[Build]") || strstr(line, "Compiling")) {
        printf("  %s%s%s\n", CLR_CYAN, line, CLR_RESET);
    } else if (strstr(line, "[Deps]") || strstr(line, "[Pre-flight]")) {
        printf("  %s%s%s\n", CLR_YELLOW, line, CLR_RESET);
    } else if (strstr(line, "[Sandbox]")) {
        printf("  %s%s%s\n", CLR_MAGENTA, line, CLR_RESET);
    } else if (strstr(line, "GFLOPS") || strstr(line, "ops/sec") || strstr(line, "ticks/sec") || strstr(line, "[PASS]") || strstr(line, "[✓]")) {
        printf("  %s%s%s\n", CLR_GREEN, line, CLR_RESET);
    } else if (strstr(line, "Error") || strstr(line, "failed") || strstr(line, "unreachable")) {
        printf("  %s%s%s\n", CLR_RED, line, CLR_RESET);
    } else if (strstr(line, "===")) {
        printf("  %s%s%s\n", CLR_BOLD, line, CLR_RESET);
    } else {
        printf("  %s%s%s\n", CLR_DIM, line, CLR_RESET);
    }
}

static void render_dashboard(BlastedTask *tasks, int count) {
    // Clear screen and move to top
    printf("\033[H\033[J");

    // Header Box
    printf("%s┌────────────────────────────────────────────────────────────────────────────────────────┐%s\n", CLR_CYAN, CLR_RESET);
    printf("%s│ %sJULY REMOTE WORKER ENGINE — CURRENT BLASTED PROCESS MONITOR (LINUX)%s               %s│%s\n",
           CLR_CYAN, CLR_BOLD, CLR_RESET, CLR_CYAN, CLR_RESET);
    printf("%s│ %sHost: Fedora Linux (x86_64)  │ Transport: Raw TCP Socket  │ Mode: tmpfs RAM Sandboxes %s│%s\n",
           CLR_CYAN, CLR_DIM, CLR_CYAN, CLR_RESET);
    printf("%s└────────────────────────────────────────────────────────────────────────────────────────┘%s\n", CLR_CYAN, CLR_RESET);

    if (count == 0) {
        printf("\n  %s[WAITING]%s No active workloads blasted from Mac yet.\n", CLR_YELLOW, CLR_RESET);
        printf("  Run %smake -j5 test-all%s or %s./dispatcher 192.168.1.41:8080%s on your Mac...\n\n",
               CLR_BOLD, CLR_RESET, CLR_BOLD, CLR_RESET);
        fflush(stdout);
        return;
    }

    // Active Processes Table
    printf("\n%s%s► CURRENT BLASTED PROCESSES (FROM MAC CLIENT):%s\n", CLR_BOLD, CLR_WHITE, CLR_RESET);
    printf("%s┌────────────────┬───────┬────────────┬─────────────────────────────┬──────────────────┐%s\n", CLR_BLUE, CLR_RESET);
    printf("%s│ TASK ID        │ PID   │ MEM (RSS)  │ WORKFLOW / TARGET           │ STATE            │%s\n", CLR_BLUE, CLR_RESET);
    printf("%s├────────────────┼───────┼────────────┼─────────────────────────────┼──────────────────┤%s\n", CLR_BLUE, CLR_RESET);

    int display_count = count > 5 ? 5 : count;
    for (int i = 0; i < display_count; i++) {
        BlastedTask *t = &tasks[i];

        char short_id[17];
        snprintf(short_id, sizeof(short_id), "%.14s..", t->task_id);

        char mem_str[16];
        if (t->rss_kb > 0) {
            snprintf(mem_str, sizeof(mem_str), "%.1f MB", (double)t->rss_kb / 1024.0);
        } else {
            snprintf(mem_str, sizeof(mem_str), "tmpfs");
        }

        const char *state_color = t->is_active ? CLR_GREEN : CLR_CYAN;
        const char *state_tag = t->is_active ? "● ACTIVE EXEC" : "✓ COMPLETED";

        char wf_display[30];
        if (t->workflow_name[0] != '\0') {
            snprintf(wf_display, sizeof(wf_display), "%-27.27s", t->workflow_name);
        } else if (t->command[0] != '\0') {
            snprintf(wf_display, sizeof(wf_display), "%-27.27s", t->command);
        } else {
            snprintf(wf_display, sizeof(wf_display), "Standard Workflow");
        }

        printf("%s│ %s%-14s %s│ %s%-5d %s│ %s%-10s %s│ %s%-27s %s│ %s%-16s%s %s│%s\n",
               CLR_BLUE, CLR_WHITE, short_id,
               CLR_BLUE, CLR_YELLOW, (int)t->pid,
               CLR_BLUE, CLR_WHITE, mem_str,
               CLR_BLUE, CLR_WHITE, wf_display,
               CLR_BLUE, state_color, state_tag, CLR_RESET,
               CLR_BLUE, CLR_RESET);
    }
    printf("%s└────────────────┴───────┴────────────┴─────────────────────────────┴──────────────────┘%s\n", CLR_BLUE, CLR_RESET);

    // Live Execution Log Stream (The centerpiece for LinkedIn screenshot!)
    printf("\n%s%s► LIVE TASK LOG STREAM (STDOUT / KERNEL DMA):%s\n", CLR_BOLD, CLR_WHITE, CLR_RESET);
    printf("%s┌────────────────────────────────────────────────────────────────────────────────────────┐%s\n", CLR_GREEN, CLR_RESET);

    BlastedTask *primary = &tasks[0];
    if (primary->log_line_count == 0) {
        printf("  %s(Waiting for process output buffer...)\n%s", CLR_DIM, CLR_RESET);
    } else {
        int start_line = 0;
        if (primary->log_line_count > 16) {
            start_line = primary->log_line_count - 16;
        }
        for (int j = start_line; j < primary->log_line_count; j++) {
            print_colorized_log_line(primary->log_lines[j]);
        }
    }
    printf("%s└────────────────────────────────────────────────────────────────────────────────────────┘%s\n", CLR_GREEN, CLR_RESET);

    printf("\n%s[July Monitor]%s Watching /tmp/july_storage and Linux /proc (Press Ctrl+C to exit)\n", CLR_DIM, CLR_RESET);
    fflush(stdout);
}

int main(int argc, char **argv) {
    bool once = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--once") == 0 || strcmp(argv[i], "-1") == 0) {
            once = true;
        }
    }

    signal(SIGINT, handle_sigint);
    signal(SIGTERM, handle_sigint);

    // Hide cursor during monitoring
    if (!once) {
        printf("\033[?25l");
    }

    BlastedTask tasks[MAX_TASKS];

    while (g_running) {
        int count = scan_july_tasks(tasks, MAX_TASKS);
        render_dashboard(tasks, count);

        if (once) break;

        // Refresh every 150 ms for ultra-smooth live view
        usleep(150000);
    }

    // Restore cursor
    printf("\033[?25h\n");
    return 0;
}
