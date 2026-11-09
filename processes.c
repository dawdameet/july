#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pwd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <ncurses.h>

#define MAX_PROCESSES 4096
#define MAX_CMDLINE_LEN 512
#define MAX_COMM_LEN 64
#define HASH_SIZE 8192

/* ---------------- Data Structures ---------------- */

typedef struct
{
    pid_t pid;
    pid_t ppid;
    uid_t uid;
    char user[32];
    char state;
    int priority;
    int nice_val;
    uint64_t utime;
    uint64_t stime;
    uint64_t starttime;
    double cpu_percent;
    uint64_t vsize_bytes;
    uint64_t rss_bytes;
    uint64_t shr_bytes;
    double mem_percent;
    uint64_t total_time_ticks;
    char comm[MAX_COMM_LEN];
    char cmdline[MAX_CMDLINE_LEN];
} ProcessInfo;

typedef struct
{
    int total_tasks;
    int running_tasks;
    int sleeping_tasks;
    int stopped_tasks;
    int zombie_tasks;
    uint64_t total_mem_kb;
    uint64_t free_mem_kb;
    uint64_t avail_mem_kb;
    uint64_t buffers_kb;
    uint64_t cached_kb;
    double loadavg[3];
    double uptime_sec;
    uint64_t total_cpu_time;
    uint64_t prev_total_cpu_time;
    int num_cores;
} SystemMetrics;

typedef enum
{
    SORT_CPU = 0,
    SORT_MEM,
    SORT_PID,
    SORT_USER,
    SORT_TIME,
    SORT_COMMAND,
    SORT_COUNT
} SortColumn;

/* CPU History Hash for Delta Calculation */
typedef struct
{
    pid_t pid;
    uint64_t utime;
    uint64_t stime;
    uint64_t sample_epoch;
    bool active;
} CpuHistoryEntry;

static CpuHistoryEntry cpu_history[HASH_SIZE];
static uint64_t current_epoch = 0;

/* UID Cache */
typedef struct
{
    uid_t uid;
    char name[32];
    bool valid;
} UidCacheEntry;

#define UID_CACHE_SIZE 256
static UidCacheEntry uid_cache[UID_CACHE_SIZE];

/* Global UI State */
static SortColumn g_sort_col = SORT_CPU;
static bool g_sort_descending = true;
static int g_scroll_offset = 0;
static int g_selected_index = 0;
static char g_filter[64] = {0};
static char g_status_msg[128] = {0};
static time_t g_status_msg_time = 0;
static bool g_paused = false;
static volatile sig_atomic_t g_sig_quit = 0;

/* ---------------- Signal & Exit Handling ---------------- */

static void handle_signal(int sig)
{
    (void)sig;
    g_sig_quit = 1;
}

/* ---------------- Caches & Lookups ---------------- */

static const char *get_cached_username(uid_t uid)
{
    size_t idx = (size_t)uid % UID_CACHE_SIZE;
    if (uid_cache[idx].valid && uid_cache[idx].uid == uid)
    {
        return uid_cache[idx].name;
    }

    struct passwd *pw = getpwuid(uid);
    uid_cache[idx].uid = uid;
    uid_cache[idx].valid = true;
    if (pw && pw->pw_name)
    {
        strncpy(uid_cache[idx].name, pw->pw_name, sizeof(uid_cache[idx].name) - 1);
        uid_cache[idx].name[sizeof(uid_cache[idx].name) - 1] = '\0';
    }
    else
    {
        snprintf(uid_cache[idx].name, sizeof(uid_cache[idx].name), "%u", (unsigned)uid);
    }
    return uid_cache[idx].name;
}

/* ---------------- Formatting Helpers ---------------- */

static void format_memory_size(uint64_t bytes, char *out, size_t out_len)
{
    double kb = (double)bytes / 1024.0;
    if (kb < 1024.0)
    {
        snprintf(out, out_len, "%4luK", (unsigned long)kb);
    }
    else if (kb < 1024.0 * 1024.0)
    {
        double mb = kb / 1024.0;
        if (mb < 100.0)
            snprintf(out, out_len, "%5.1fM", mb);
        else
            snprintf(out, out_len, "%5.0fM", mb);
    }
    else
    {
        double gb = kb / (1024.0 * 1024.0);
        snprintf(out, out_len, "%5.1fG", gb);
    }
}

static void format_time_ticks(uint64_t ticks, long clk_tck, char *out, size_t out_len)
{
    if (out_len == 0) return;
    uint64_t total_sec = clk_tck > 0 ? (ticks / (uint64_t)clk_tck) : 0;
    uint64_t centiseconds = clk_tck > 0 ? ((ticks % (uint64_t)clk_tck) * 100 / (uint64_t)clk_tck) : 0;
    uint64_t minutes = total_sec / 60;
    uint64_t seconds = total_sec % 60;
    uint64_t hours = minutes / 60;

    if (hours > 0)
    {
        snprintf(out, out_len, "%lu:%02lu:%02lu", (unsigned long)hours, (unsigned long)(minutes % 60), (unsigned long)seconds);
    }
    else
    {
        snprintf(out, out_len, "%lu:%02lu.%02lu", (unsigned long)minutes, (unsigned long)seconds, (unsigned long)centiseconds);
    }
}

/* ---------------- System Metrics Collection ---------------- */

static void read_system_metrics(SystemMetrics *sys)
{
    memset(sys, 0, sizeof(*sys));
    sys->num_cores = get_nprocs();
    if (sys->num_cores <= 0)
        sys->num_cores = 1;

    // Read Uptime
    FILE *uf = fopen("/proc/uptime", "r");
    if (uf)
    {
        if (fscanf(uf, "%lf", &sys->uptime_sec) != 1)
            sys->uptime_sec = 0;
        fclose(uf);
    }

    // Read Load Average
    FILE *lf = fopen("/proc/loadavg", "r");
    if (lf)
    {
        if (fscanf(lf, "%lf %lf %lf", &sys->loadavg[0], &sys->loadavg[1], &sys->loadavg[2]) != 3)
        {
            sys->loadavg[0] = sys->loadavg[1] = sys->loadavg[2] = 0.0;
        }
        fclose(lf);
    }

    // Read Memory Info
    FILE *mf = fopen("/proc/meminfo", "r");
    if (mf)
    {
        char line[256];
        while (fgets(line, sizeof(line), mf))
        {
            uint64_t val = 0;
            if (sscanf(line, "MemTotal: %lu kB", &val) == 1)
                sys->total_mem_kb = val;
            else if (sscanf(line, "MemFree: %lu kB", &val) == 1)
                sys->free_mem_kb = val;
            else if (sscanf(line, "MemAvailable: %lu kB", &val) == 1)
                sys->avail_mem_kb = val;
            else if (sscanf(line, "Buffers: %lu kB", &val) == 1)
                sys->buffers_kb = val;
            else if (sscanf(line, "Cached: %lu kB", &val) == 1)
                sys->cached_kb = val;
        }
        fclose(mf);
    }

    // Read Total CPU Jiffies
    FILE *sf = fopen("/proc/stat", "r");
    if (sf)
    {
        char line[512];
        if (fgets(line, sizeof(line), sf) && strncmp(line, "cpu ", 4) == 0)
        {
            uint64_t user, nice, system_val, idle, iowait, irq, softirq, steal;
            int n = sscanf(line + 4, "%lu %lu %lu %lu %lu %lu %lu %lu",
                           &user, &nice, &system_val, &idle, &iowait, &irq, &softirq, &steal);
            if (n >= 4)
            {
                sys->total_cpu_time = user + nice + system_val + idle;
                if (n >= 5) sys->total_cpu_time += iowait;
                if (n >= 6) sys->total_cpu_time += irq;
                if (n >= 7) sys->total_cpu_time += softirq;
                if (n >= 8) sys->total_cpu_time += steal;
            }
        }
        fclose(sf);
    }
}

/* ---------------- Process Scanning ---------------- */

static int scan_processes(ProcessInfo *procs, int max_procs, SystemMetrics *sys,
                          uint64_t prev_cpu_time, uint64_t curr_cpu_time)
{
    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0)
        clk_tck = 100;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0)
        page_size = 4096;

    current_epoch++;
    uint64_t delta_sys_cpu = (curr_cpu_time > prev_cpu_time) ? (curr_cpu_time - prev_cpu_time) : 0;

    DIR *dir = opendir("/proc");
    if (!dir)
        return 0;

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && count < max_procs)
    {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;

        pid_t pid = (pid_t)atoi(entry->d_name);
        if (pid <= 0)
            continue;

        char path[256];

        // 1. Read /proc/<pid>/stat
        snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        int sfd = open(path, O_RDONLY);
        if (sfd < 0)
            continue;

        char sbuf[1024];
        ssize_t sn = read(sfd, sbuf, sizeof(sbuf) - 1);
        close(sfd);
        if (sn <= 0)
            continue;
        sbuf[sn] = '\0';

        char *open_p = strchr(sbuf, '(');
        char *close_p = strrchr(sbuf, ')');
        if (!open_p || !close_p || close_p < open_p)
            continue;

        ProcessInfo *p = &procs[count];
        memset(p, 0, sizeof(*p));
        p->pid = pid;

        // Extract comm
        size_t comm_len = (size_t)(close_p - open_p - 1);
        if (comm_len >= sizeof(p->comm))
            comm_len = sizeof(p->comm) - 1;
        memcpy(p->comm, open_p + 1, comm_len);
        p->comm[comm_len] = '\0';

        // Parse fields after ')'
        char *rest = close_p + 2;
        int ppid_val, pgrp_val, session_val, tty_nr, tpgid_val;
        unsigned int flags;
        unsigned long minflt, cminflt, majflt, cmajflt, utime_val, stime_val;
        long cutime_val, cstime_val, priority_val, nice_val, num_threads_val, itrealvalue;
        unsigned long long starttime_val;
        unsigned long vsize_val;
        long rss_pages;

        int fields_read = sscanf(rest,
            "%c %d %d %d %d %d %u %lu %lu %lu %lu %lu %lu %ld %ld %ld %ld %ld %ld %llu %lu %ld",
            &p->state, &ppid_val, &pgrp_val, &session_val, &tty_nr, &tpgid_val,
            &flags, &minflt, &cminflt, &majflt, &cmajflt,
            &utime_val, &stime_val, &cutime_val, &cstime_val,
            &priority_val, &nice_val, &num_threads_val, &itrealvalue,
            &starttime_val, &vsize_val, &rss_pages);

        if (fields_read < 22)
            continue;

        p->ppid = ppid_val;
        p->priority = (int)priority_val;
        p->nice_val = (int)nice_val;
        p->utime = utime_val;
        p->stime = stime_val;
        p->starttime = starttime_val;
        p->total_time_ticks = utime_val + stime_val;
        p->vsize_bytes = (uint64_t)vsize_val;
        p->rss_bytes = rss_pages > 0 ? (uint64_t)rss_pages * (uint64_t)page_size : 0;

        // 2. Read /proc/<pid>/statm for shared memory
        snprintf(path, sizeof(path), "/proc/%d/statm", pid);
        FILE *smf = fopen(path, "r");
        if (smf)
        {
            unsigned long sm_size, sm_res, sm_shr;
            if (fscanf(smf, "%lu %lu %lu", &sm_size, &sm_res, &sm_shr) == 3)
            {
                p->shr_bytes = sm_shr * (uint64_t)page_size;
            }
            fclose(smf);
        }

        // 3. Read /proc/<pid>/status for UID
        snprintf(path, sizeof(path), "/proc/%d/status", pid);
        FILE *stf = fopen(path, "r");
        p->uid = 0;
        if (stf)
        {
            char line[256];
            while (fgets(line, sizeof(line), stf))
            {
                if (strncmp(line, "Uid:", 4) == 0)
                {
                    unsigned int ruid, euid;
                    if (sscanf(line + 4, "%u %u", &ruid, &euid) >= 2)
                    {
                        p->uid = (uid_t)euid;
                    }
                    break;
                }
            }
            fclose(stf);
        }
        strncpy(p->user, get_cached_username(p->uid), sizeof(p->user) - 1);

        // 4. Read /proc/<pid>/cmdline
        snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
        int cfd = open(path, O_RDONLY);
        if (cfd >= 0)
        {
            ssize_t cn = read(cfd, p->cmdline, sizeof(p->cmdline) - 1);
            close(cfd);
            if (cn > 0)
            {
                for (ssize_t i = 0; i < cn - 1; i++)
                {
                    if (p->cmdline[i] == '\0')
                        p->cmdline[i] = ' ';
                }
                p->cmdline[cn] = '\0';
            }
            else
            {
                snprintf(p->cmdline, sizeof(p->cmdline), "[%s]", p->comm);
            }
        }
        else
        {
            snprintf(p->cmdline, sizeof(p->cmdline), "[%s]", p->comm);
        }

        // Calculate Memory %
        if (sys->total_mem_kb > 0)
        {
            p->mem_percent = ((double)p->rss_bytes / (double)(sys->total_mem_kb * 1024.0)) * 100.0;
        }

        // Calculate CPU %
        size_t h_idx = (size_t)pid % HASH_SIZE;
        size_t start_h = h_idx;
        CpuHistoryEntry *he = NULL;
        while (cpu_history[h_idx].active)
        {
            if (cpu_history[h_idx].pid == pid)
            {
                he = &cpu_history[h_idx];
                break;
            }
            h_idx = (h_idx + 1) % HASH_SIZE;
            if (h_idx == start_h)
                break;
        }

        if (he != NULL && delta_sys_cpu > 0 && he->sample_epoch == current_epoch - 1)
        {
            uint64_t prev_proc = he->utime + he->stime;
            uint64_t curr_proc = p->utime + p->stime;
            uint64_t delta_proc = (curr_proc > prev_proc) ? (curr_proc - prev_proc) : 0;
            p->cpu_percent = ((double)delta_proc / (double)delta_sys_cpu) * 100.0 * sys->num_cores;
        }
        else
        {
            // First sample or fallback: average over process lifetime
            double proc_sec = sys->uptime_sec - ((double)p->starttime / (double)clk_tck);
            if (proc_sec > 0.05)
            {
                p->cpu_percent = (((double)p->total_time_ticks / (double)clk_tck) / proc_sec) * 100.0;
            }
            else
            {
                p->cpu_percent = 0.0;
            }
        }

        if (p->cpu_percent > 100.0 * sys->num_cores)
            p->cpu_percent = 100.0 * sys->num_cores;
        if (p->cpu_percent < 0.0)
            p->cpu_percent = 0.0;

        // Record in CPU history
        if (!he)
        {
            h_idx = (size_t)pid % HASH_SIZE;
            while (cpu_history[h_idx].active && cpu_history[h_idx].pid != pid &&
                   cpu_history[h_idx].sample_epoch == current_epoch)
            {
                h_idx = (h_idx + 1) % HASH_SIZE;
            }
            he = &cpu_history[h_idx];
            he->pid = pid;
            he->active = true;
        }
        he->utime = p->utime;
        he->stime = p->stime;
        he->sample_epoch = current_epoch;

        // Update task state counts
        sys->total_tasks++;
        switch (p->state)
        {
            case 'R': sys->running_tasks++; break;
            case 'S':
            case 'D': sys->sleeping_tasks++; break;
            case 'T':
            case 't': sys->stopped_tasks++; break;
            case 'Z': sys->zombie_tasks++; break;
            default: break;
        }

        count++;
    }

    closedir(dir);

    // Evict old hash entries
    for (size_t i = 0; i < HASH_SIZE; i++)
    {
        if (cpu_history[i].active && cpu_history[i].sample_epoch < current_epoch - 2)
        {
            cpu_history[i].active = false;
        }
    }

    return count;
}

/* ---------------- Sorting ---------------- */

static int compare_processes(const void *a, const void *b)
{
    const ProcessInfo *p1 = (const ProcessInfo *)a;
    const ProcessInfo *p2 = (const ProcessInfo *)b;

    int diff = 0;
    switch (g_sort_col)
    {
        case SORT_CPU:
            if (p1->cpu_percent < p2->cpu_percent) diff = -1;
            else if (p1->cpu_percent > p2->cpu_percent) diff = 1;
            else diff = (p1->pid < p2->pid) ? -1 : 1;
            break;
        case SORT_MEM:
            if (p1->rss_bytes < p2->rss_bytes) diff = -1;
            else if (p1->rss_bytes > p2->rss_bytes) diff = 1;
            else diff = (p1->pid < p2->pid) ? -1 : 1;
            break;
        case SORT_PID:
            diff = (p1->pid < p2->pid) ? -1 : 1;
            break;
        case SORT_USER:
            diff = strcmp(p1->user, p2->user);
            if (diff == 0) diff = (p1->pid < p2->pid) ? -1 : 1;
            break;
        case SORT_TIME:
            if (p1->total_time_ticks < p2->total_time_ticks) diff = -1;
            else if (p1->total_time_ticks > p2->total_time_ticks) diff = 1;
            else diff = (p1->pid < p2->pid) ? -1 : 1;
            break;
        case SORT_COMMAND:
            diff = strcasecmp(p1->comm, p2->comm);
            if (diff == 0) diff = (p1->pid < p2->pid) ? -1 : 1;
            break;
        default:
            break;
    }

    return g_sort_descending ? -diff : diff;
}

/* ---------------- Filtering ---------------- */

static bool process_matches_filter(const ProcessInfo *p, const char *filt)
{
    if (!filt || filt[0] == '\0')
        return true;

    char pid_str[32];
    snprintf(pid_str, sizeof(pid_str), "%d", p->pid);
    if (strstr(pid_str, filt)) return true;
    if (strcasestr(p->user, filt)) return true;
    if (strcasestr(p->comm, filt)) return true;
    if (strcasestr(p->cmdline, filt)) return true;
    return false;
}

/* ---------------- Non-Interactive / Batch Mode ---------------- */

static void print_batch_snapshot(ProcessInfo *procs, int count, const SystemMetrics *sys, long clk_tck)
{
    printf("JULY PROCESS SNAPSHOT - Tasks: %d total, %d running, %d sleeping, %d stopped, %d zombie\n",
           sys->total_tasks, sys->running_tasks, sys->sleeping_tasks, sys->stopped_tasks, sys->zombie_tasks);
    printf("Load average: %.2f, %.2f, %.2f | Cores: %d | Mem: %.1fG/%.1fG (%.1f%%)\n\n",
           sys->loadavg[0], sys->loadavg[1], sys->loadavg[2], sys->num_cores,
           (double)(sys->total_mem_kb - sys->avail_mem_kb) / 1048576.0,
           (double)sys->total_mem_kb / 1048576.0,
           sys->total_mem_kb > 0 ? (100.0 * (sys->total_mem_kb - sys->avail_mem_kb) / sys->total_mem_kb) : 0.0);

    printf("%-7s %-12s %3s %3s %7s %7s %7s %1s %5s %5s %9s  %s\n",
           "PID", "USER", "PR", "NI", "VIRT", "RES", "SHR", "S", "%CPU", "%MEM", "TIME+", "COMMAND");
    printf("----------------------------------------------------------------------------------------------------\n");

    for (int i = 0; i < count; i++)
    {
        ProcessInfo *p = &procs[i];
        if (g_filter[0] && !process_matches_filter(p, g_filter))
            continue;

        char virt_str[32], res_str[32], shr_str[32], time_str[32];
        format_memory_size(p->vsize_bytes, virt_str, sizeof(virt_str));
        format_memory_size(p->rss_bytes, res_str, sizeof(res_str));
        format_memory_size(p->shr_bytes, shr_str, sizeof(shr_str));
        format_time_ticks(p->total_time_ticks, clk_tck, time_str, sizeof(time_str));

        printf("%-7d %-12.12s %3d %3d %7s %7s %7s %c %5.1f %5.1f %9s  %s\n",
               p->pid, p->user, p->priority, p->nice_val,
               virt_str, res_str, shr_str, p->state,
               p->cpu_percent, p->mem_percent, time_str, p->cmdline);
    }
}

/* ---------------- Interactive TUI (ncurses) ---------------- */

#define COLOR_PAIR_HEADER    1
#define COLOR_PAIR_COL_HEAD  2
#define COLOR_PAIR_RUNNING   3
#define COLOR_PAIR_SLEEPING  4
#define COLOR_PAIR_ZOMBIE    5
#define COLOR_PAIR_SELECTED  6
#define COLOR_PAIR_HOT       7
#define COLOR_PAIR_FOOTER    8

static void init_ncurses_colors(void)
{
    if (has_colors())
    {
        start_color();
        use_default_colors();
        init_pair(COLOR_PAIR_HEADER, COLOR_CYAN, -1);
        init_pair(COLOR_PAIR_COL_HEAD, COLOR_BLACK, COLOR_CYAN);
        init_pair(COLOR_PAIR_RUNNING, COLOR_GREEN, -1);
        init_pair(COLOR_PAIR_SLEEPING, COLOR_WHITE, -1);
        init_pair(COLOR_PAIR_ZOMBIE, COLOR_RED, -1);
        init_pair(COLOR_PAIR_SELECTED, COLOR_BLACK, COLOR_WHITE);
        init_pair(COLOR_PAIR_HOT, COLOR_YELLOW, -1);
        init_pair(COLOR_PAIR_FOOTER, COLOR_BLACK, COLOR_GREEN);
    }
}

static void render_progress_bar(char *buf, size_t buf_len, double percent, int width)
{
    if (width <= 0) { buf[0] = '\0'; return; }
    if (percent < 0.0) percent = 0.0;
    if (percent > 100.0) percent = 100.0;

    int filled = (int)((percent / 100.0) * width);
    if (filled > width) filled = width;

    int pos = 0;
    buf[pos++] = '[';
    for (int i = 0; i < width && pos < (int)buf_len - 2; i++)
    {
        buf[pos++] = (i < filled) ? '|' : ' ';
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
}

static const char *get_sort_column_name(SortColumn col)
{
    switch (col)
    {
        case SORT_CPU:     return "%CPU";
        case SORT_MEM:     return "%MEM";
        case SORT_PID:     return "PID";
        case SORT_USER:    return "USER";
        case SORT_TIME:    return "TIME+";
        case SORT_COMMAND: return "COMMAND";
        default:           return "UNKNOWN";
    }
}

static void draw_tui(const SystemMetrics *sys, ProcessInfo *filtered_procs, int filtered_count,
                     int total_count, long clk_tck)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    erase();

    // Line 0: Header title & System state
    attron(COLOR_PAIR(COLOR_PAIR_HEADER) | A_BOLD);
    mvprintw(0, 0, " JULY PROCESS MONITOR ");
    attroff(COLOR_PAIR(COLOR_PAIR_HEADER) | A_BOLD);

    printw(" Tasks: %d total, %d running, %d sleeping, %d stopped, %d zombie",
           sys->total_tasks, sys->running_tasks, sys->sleeping_tasks, sys->stopped_tasks, sys->zombie_tasks);

    // Line 1: CPU Usage & Cores
    double cpu_usage = 0.0;
    // Calculate global CPU usage across all tasks
    for (int i = 0; i < total_count; i++)
    {
        cpu_usage += filtered_procs[i].cpu_percent;
    }
    double cpu_norm = cpu_usage / sys->num_cores;
    if (cpu_norm > 100.0) cpu_norm = 100.0;

    char cpu_bar[64];
    render_progress_bar(cpu_bar, sizeof(cpu_bar), cpu_norm, 24);
    mvprintw(1, 0, "CPU %s %5.1f%%  Cores: %d  Load: %.2f, %.2f, %.2f",
             cpu_bar, cpu_norm, sys->num_cores, sys->loadavg[0], sys->loadavg[1], sys->loadavg[2]);

    // Line 2: Memory Usage
    uint64_t used_mem = (sys->total_mem_kb > sys->avail_mem_kb) ? (sys->total_mem_kb - sys->avail_mem_kb) : 0;
    double mem_pct = sys->total_mem_kb > 0 ? (100.0 * (double)used_mem / (double)sys->total_mem_kb) : 0.0;
    char mem_bar[64];
    render_progress_bar(mem_bar, sizeof(mem_bar), mem_pct, 24);
    mvprintw(2, 0, "Mem %s %5.1f%%  Used: %.1fG / %.1fG  Avail: %.1fG",
             mem_bar, mem_pct, (double)used_mem / 1048576.0, (double)sys->total_mem_kb / 1048576.0,
             (double)sys->avail_mem_kb / 1048576.0);

    // Line 3: Status / Filter / Sort Bar
    uint64_t uptime_d = (uint64_t)sys->uptime_sec / 86400;
    uint64_t uptime_h = ((uint64_t)sys->uptime_sec % 86400) / 3600;
    uint64_t uptime_m = ((uint64_t)sys->uptime_sec % 3600) / 60;

    mvprintw(3, 0, "Uptime: %lud %02lu:%02lu  |  Sort: %s%s  |  Showing: %d/%d",
             (unsigned long)uptime_d, (unsigned long)uptime_h, (unsigned long)uptime_m,
             get_sort_column_name(g_sort_col), g_sort_descending ? "▼" : "▲",
             filtered_count, total_count);

    if (g_filter[0])
    {
        printw("  |  Filter: \"%s\"", g_filter);
    }
    if (g_paused)
    {
        attron(A_BOLD | COLOR_PAIR(COLOR_PAIR_ZOMBIE));
        printw("  [PAUSED]");
        attroff(A_BOLD | COLOR_PAIR(COLOR_PAIR_ZOMBIE));
    }

    // Line 4: Column Headers
    int header_row = 4;
    attron(COLOR_PAIR(COLOR_PAIR_COL_HEAD) | A_BOLD);
    mvprintw(header_row, 0, "%-7s %-12s %3s %3s %7s %7s %7s %1s %5s %5s %9s  %-*s",
             (g_sort_col == SORT_PID) ? (g_sort_descending ? "PID▼" : "PID▲") : "PID",
             (g_sort_col == SORT_USER) ? (g_sort_descending ? "USER▼" : "USER▲") : "USER",
             "PR", "NI", "VIRT", "RES", "SHR", "S",
             (g_sort_col == SORT_CPU) ? (g_sort_descending ? "%CPU▼" : "%CPU▲") : "%CPU",
             (g_sort_col == SORT_MEM) ? (g_sort_descending ? "%MEM▼" : "%MEM▲") : "%MEM",
             (g_sort_col == SORT_TIME) ? (g_sort_descending ? "TIME+▼" : "TIME+▲") : "TIME+",
             max_x - 69 > 7 ? max_x - 69 : 7,
             (g_sort_col == SORT_COMMAND) ? (g_sort_descending ? "COMMAND▼" : "COMMAND▲") : "COMMAND");
    attroff(COLOR_PAIR(COLOR_PAIR_COL_HEAD) | A_BOLD);

    // Process List Rows
    int list_start_y = 5;
    int list_height = max_y - list_start_y - 1; // reserve 1 line for footer
    if (list_height < 1)
        list_height = 1;

    // Adjust scrolling bounds
    if (g_selected_index < 0)
        g_selected_index = 0;
    if (g_selected_index >= filtered_count && filtered_count > 0)
        g_selected_index = filtered_count - 1;

    if (g_selected_index < g_scroll_offset)
        g_scroll_offset = g_selected_index;
    if (g_selected_index >= g_scroll_offset + list_height)
        g_scroll_offset = g_selected_index - list_height + 1;

    for (int i = 0; i < list_height; i++)
    {
        int p_idx = g_scroll_offset + i;
        int row_y = list_start_y + i;

        if (p_idx >= filtered_count)
        {
            move(row_y, 0);
            clrtoeol();
            continue;
        }

        ProcessInfo *p = &filtered_procs[p_idx];
        bool is_selected = (p_idx == g_selected_index);

        char virt_str[32], res_str[32], shr_str[32], time_str[32];
        format_memory_size(p->vsize_bytes, virt_str, sizeof(virt_str));
        format_memory_size(p->rss_bytes, res_str, sizeof(res_str));
        format_memory_size(p->shr_bytes, shr_str, sizeof(shr_str));
        format_time_ticks(p->total_time_ticks, clk_tck, time_str, sizeof(time_str));

        if (is_selected)
        {
            attron(COLOR_PAIR(COLOR_PAIR_SELECTED) | A_BOLD);
        }
        else
        {
            if (p->state == 'R')
                attron(COLOR_PAIR(COLOR_PAIR_RUNNING) | A_BOLD);
            else if (p->state == 'Z')
                attron(COLOR_PAIR(COLOR_PAIR_ZOMBIE) | A_BOLD);
            else if (p->cpu_percent > 20.0 || p->mem_percent > 10.0)
                attron(COLOR_PAIR(COLOR_PAIR_HOT));
            else
                attron(COLOR_PAIR(COLOR_PAIR_SLEEPING));
        }

        // Format command to fill available terminal width
        int cmd_avail = max_x - 68;
        if (cmd_avail < 4) cmd_avail = 4;
        char cmd_display[512];
        snprintf(cmd_display, sizeof(cmd_display), "%.*s", cmd_avail, p->cmdline);

        mvprintw(row_y, 0, "%-7d %-12.12s %3d %3d %7s %7s %7s %c %5.1f %5.1f %9s %-*s",
                 p->pid, p->user, p->priority, p->nice_val,
                 virt_str, res_str, shr_str, p->state,
                 p->cpu_percent, p->mem_percent, time_str,
                 cmd_avail, cmd_display);

        if (is_selected)
        {
            attroff(COLOR_PAIR(COLOR_PAIR_SELECTED) | A_BOLD);
        }
        else
        {
            if (p->state == 'R')
                attroff(COLOR_PAIR(COLOR_PAIR_RUNNING) | A_BOLD);
            else if (p->state == 'Z')
                attroff(COLOR_PAIR(COLOR_PAIR_ZOMBIE) | A_BOLD);
            else if (p->cpu_percent > 20.0 || p->mem_percent > 10.0)
                attroff(COLOR_PAIR(COLOR_PAIR_HOT));
            else
                attroff(COLOR_PAIR(COLOR_PAIR_SLEEPING));
        }
    }

    // Line max_y - 1: Footer status / prompt
    int footer_y = max_y - 1;
    time_t now = time(NULL);

    if (g_status_msg[0] && now - g_status_msg_time < 3)
    {
        attron(COLOR_PAIR(COLOR_PAIR_FOOTER) | A_BOLD);
        mvprintw(footer_y, 0, " >> %-*s", max_x - 5, g_status_msg);
        attroff(COLOR_PAIR(COLOR_PAIR_FOOTER) | A_BOLD);
    }
    else
    {
        attron(COLOR_PAIR(COLOR_PAIR_FOOTER));
        mvprintw(footer_y, 0, " [q]Quit  [s]Sort  [r]Reverse  [/]Filter  [k]Kill  [Space]Pause  [↑↓]Select  [PgUp/Dn]Scroll ");
        attroff(COLOR_PAIR(COLOR_PAIR_FOOTER));
    }

    refresh();
}

static void prompt_kill_process(pid_t pid, const char *comm)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_x;

    echo();
    curs_set(1);
    timeout(-1); // Blocking read for prompt

    attron(COLOR_PAIR(COLOR_PAIR_COL_HEAD) | A_BOLD);
    mvprintw(max_y - 1, 0, " Kill PID %d (%s)? Enter signal [default 15 (SIGTERM), 9 (SIGKILL), 'c' cancel]: ", pid, comm);
    clrtoeol();
    attroff(COLOR_PAIR(COLOR_PAIR_COL_HEAD) | A_BOLD);

    char sig_str[32] = {0};
    getnstr(sig_str, sizeof(sig_str) - 1);

    noecho();
    curs_set(0);
    timeout(1000); // Restore 1s timeout

    if (sig_str[0] == 'c' || sig_str[0] == 'C')
    {
        snprintf(g_status_msg, sizeof(g_status_msg), "Kill canceled.");
        g_status_msg_time = time(NULL);
        return;
    }

    int sig = SIGTERM;
    if (sig_str[0] != '\0')
    {
        sig = atoi(sig_str);
        if (sig <= 0 || sig > 64)
            sig = SIGTERM;
    }

    if (kill(pid, sig) == 0)
    {
        snprintf(g_status_msg, sizeof(g_status_msg), "Signal %d sent to PID %d (%s).", sig, pid, comm);
    }
    else
    {
        snprintf(g_status_msg, sizeof(g_status_msg), "Failed to signal PID %d: %s", pid, strerror(errno));
    }
    g_status_msg_time = time(NULL);
}

static void prompt_filter_string(void)
{
    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);
    (void)max_x;

    echo();
    curs_set(1);
    timeout(-1);

    attron(COLOR_PAIR(COLOR_PAIR_COL_HEAD) | A_BOLD);
    mvprintw(max_y - 1, 0, " Filter (empty to clear): ");
    clrtoeol();
    attroff(COLOR_PAIR(COLOR_PAIR_COL_HEAD) | A_BOLD);

    char new_filt[64] = {0};
    getnstr(new_filt, sizeof(new_filt) - 1);

    noecho();
    curs_set(0);
    timeout(1000);

    strncpy(g_filter, new_filt, sizeof(g_filter) - 1);
    g_filter[sizeof(g_filter) - 1] = '\0';
    g_selected_index = 0;
    g_scroll_offset = 0;

    if (g_filter[0])
    {
        snprintf(g_status_msg, sizeof(g_status_msg), "Filter set to '%s'.", g_filter);
    }
    else
    {
        snprintf(g_status_msg, sizeof(g_status_msg), "Filter cleared.");
    }
    g_status_msg_time = time(NULL);
}

/* ---------------- Main Entry Point ---------------- */

int main(int argc, char **argv)
{
    bool batch_mode = false;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--once") == 0 || strcmp(argv[i], "-1") == 0 ||
            strcmp(argv[i], "--batch") == 0 || strcmp(argv[i], "-b") == 0)
        {
            batch_mode = true;
        }
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0)
        {
            printf("Usage: %s [options]\n", argv[0]);
            printf("High-Performance Linux Process Monitor\n\n");
            printf("Options:\n");
            printf("  -1, --once, -b, --batch    Print process list snapshot and exit\n");
            printf("  -f, --filter <str>         Filter processes by name or PID\n");
            printf("  -s, --sort <col>           Sort by: cpu, mem, pid, user, time, comm\n");
            printf("  -h, --help                 Display this help message\n\n");
            printf("Interactive TUI Controls:\n");
            printf("  q, F10, Esc               Quit\n");
            printf("  s, F6                     Cycle sort column (CPU, MEM, PID, USER, TIME)\n");
            printf("  r                         Reverse sort order\n");
            printf("  /, f                      Filter processes\n");
            printf("  c                         Clear filter\n");
            printf("  k                         Send signal to selected process\n");
            printf("  Space                     Pause / resume auto-refresh\n");
            printf("  Up/Down, j/k              Navigate list\n");
            printf("  PageUp/PageDown, Home/End Fast scroll\n");
            return 0;
        }
        else if ((strcmp(argv[i], "-f") == 0 || strcmp(argv[i], "--filter") == 0) && i + 1 < argc)
        {
            strncpy(g_filter, argv[++i], sizeof(g_filter) - 1);
        }
        else if ((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--sort") == 0) && i + 1 < argc)
        {
            i++;
            if (strcasecmp(argv[i], "cpu") == 0) g_sort_col = SORT_CPU;
            else if (strcasecmp(argv[i], "mem") == 0) g_sort_col = SORT_MEM;
            else if (strcasecmp(argv[i], "pid") == 0) g_sort_col = SORT_PID;
            else if (strcasecmp(argv[i], "user") == 0) g_sort_col = SORT_USER;
            else if (strcasecmp(argv[i], "time") == 0) g_sort_col = SORT_TIME;
            else if (strcasecmp(argv[i], "comm") == 0) g_sort_col = SORT_COMMAND;
        }
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
    {
        batch_mode = true;
    }

    long clk_tck = sysconf(_SC_CLK_TCK);
    if (clk_tck <= 0) clk_tck = 100;

    static ProcessInfo raw_procs[MAX_PROCESSES];
    static ProcessInfo filtered_procs[MAX_PROCESSES];
    SystemMetrics sys;

    uint64_t prev_cpu_time = 0;
    read_system_metrics(&sys);
    prev_cpu_time = sys.total_cpu_time;

    // First scan to initialize history
    int count = scan_processes(raw_procs, MAX_PROCESSES, &sys, 0, prev_cpu_time);

    // If batch mode, sleep 100ms and sample again for accurate CPU %
    if (batch_mode)
    {
        usleep(100000); // 100ms
        uint64_t curr_cpu = 0;
        read_system_metrics(&sys);
        curr_cpu = sys.total_cpu_time;
        count = scan_processes(raw_procs, MAX_PROCESSES, &sys, prev_cpu_time, curr_cpu);

        qsort(raw_procs, (size_t)count, sizeof(ProcessInfo), compare_processes);
        print_batch_snapshot(raw_procs, count, &sys, clk_tck);
        return 0;
    }

    // Interactive TUI Mode with ncurses
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(1000); // 1.0s non-blocking refresh
    init_ncurses_colors();

    while (!g_sig_quit)
    {
        uint64_t curr_cpu = 0;
        if (!g_paused)
        {
            read_system_metrics(&sys);
            curr_cpu = sys.total_cpu_time;
            count = scan_processes(raw_procs, MAX_PROCESSES, &sys, prev_cpu_time, curr_cpu);
            prev_cpu_time = curr_cpu;
        }

        // Apply filter
        int filtered_count = 0;
        for (int i = 0; i < count; i++)
        {
            if (process_matches_filter(&raw_procs[i], g_filter))
            {
                filtered_procs[filtered_count++] = raw_procs[i];
            }
        }

        // Sort filtered list
        qsort(filtered_procs, (size_t)filtered_count, sizeof(ProcessInfo), compare_processes);

        // Draw Screen
        draw_tui(&sys, filtered_procs, filtered_count, count, clk_tck);

        // Input handling
        int ch = getch();
        if (ch == ERR)
            continue; // Timeout, refresh loop

        switch (ch)
        {
            case 'q':
            case 'Q':
            case 27: // ESC
            case KEY_F(10):
                g_sig_quit = 1;
                break;

            case KEY_UP:
            case 'k':
                if (g_selected_index > 0)
                    g_selected_index--;
                break;

            case KEY_DOWN:
            case 'j':
                if (g_selected_index < filtered_count - 1)
                    g_selected_index++;
                break;

            case KEY_PPAGE: // Page Up
                g_selected_index -= 15;
                if (g_selected_index < 0) g_selected_index = 0;
                break;

            case KEY_NPAGE: // Page Down
                g_selected_index += 15;
                if (g_selected_index >= filtered_count && filtered_count > 0)
                    g_selected_index = filtered_count - 1;
                break;

            case KEY_HOME:
            case 'g':
                g_selected_index = 0;
                g_scroll_offset = 0;
                break;

            case KEY_END:
            case 'G':
                if (filtered_count > 0)
                    g_selected_index = filtered_count - 1;
                break;

            case 's':
            case 'S':
            case KEY_F(6):
                g_sort_col = (g_sort_col + 1) % SORT_COUNT;
                g_selected_index = 0;
                g_scroll_offset = 0;
                snprintf(g_status_msg, sizeof(g_status_msg), "Sorted by %s.", get_sort_column_name(g_sort_col));
                g_status_msg_time = time(NULL);
                break;

            case 'r':
            case 'R':
                g_sort_descending = !g_sort_descending;
                snprintf(g_status_msg, sizeof(g_status_msg), "Sort order: %s.", g_sort_descending ? "Descending" : "Ascending");
                g_status_msg_time = time(NULL);
                break;

            case ' ':
                g_paused = !g_paused;
                snprintf(g_status_msg, sizeof(g_status_msg), g_paused ? "Monitoring paused." : "Monitoring resumed.");
                g_status_msg_time = time(NULL);
                break;

            case '/':
            case 'f':
                prompt_filter_string();
                break;

            case 'c':
            case 'C':
                g_filter[0] = '\0';
                g_selected_index = 0;
                g_scroll_offset = 0;
                snprintf(g_status_msg, sizeof(g_status_msg), "Filter cleared.");
                g_status_msg_time = time(NULL);
                break;

            case 'x':
            case 'K': // Kill selected
                if (filtered_count > 0 && g_selected_index < filtered_count)
                {
                    prompt_kill_process(filtered_procs[g_selected_index].pid,
                                        filtered_procs[g_selected_index].comm);
                }
                break;

            default:
                break;
        }
    }

    endwin();
    return 0;
}
