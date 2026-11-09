#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sys/time.h>

#define PRIME_LIMIT 500000
#define NUM_BENCH_THREADS 4

typedef struct {
    int thread_id;
    uint64_t count;
    double elapsed_ms;
} ThreadResult;

static bool is_prime(uint64_t n) {
    if (n < 2) return false;
    if (n == 2 || n == 3) return true;
    if (n % 2 == 0 || n % 3 == 0) return false;
    for (uint64_t i = 5; i * i <= n; i += 6) {
        if (n % i == 0 || n % (i + 2) == 0) return false;
    }
    return true;
}

static void *worker_func(void *arg) {
    ThreadResult *res = (ThreadResult *)arg;
    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    uint64_t count = 0;
    for (uint64_t i = 2; i <= PRIME_LIMIT; i++) {
        if (is_prime(i)) {
            count++;
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &end);
    res->count = count;
    res->elapsed_ms = ((double)(end.tv_sec - start.tv_sec) * 1000.0) +
                      ((double)(end.tv_nsec - start.tv_nsec) / 1e6);
    return NULL;
}

static void read_first_line(const char *path, char *buf, size_t max_len) {
    buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(buf, max_len, f)) {
        size_t l = strlen(buf);
        while (l > 0 && (buf[l - 1] == '\n' || buf[l - 1] == '\r')) {
            buf[--l] = '\0';
        }
    }
    fclose(f);
}

static void get_cpu_model(char *buf, size_t max_len) {
    buf[0] = '\0';
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) {
        strncpy(buf, "Unknown / Non-Linux", max_len - 1);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "model name", 10) == 0) {
            char *colon = strchr(line, ':');
            if (colon) {
                colon++;
                while (*colon == ' ' || *colon == '\t') colon++;
                size_t l = strlen(colon);
                while (l > 0 && (colon[l - 1] == '\n' || colon[l - 1] == '\r')) {
                    colon[--l] = '\0';
                }
                strncpy(buf, colon, max_len - 1);
                break;
            }
        }
    }
    fclose(f);
    if (buf[0] == '\0') {
        strncpy(buf, "Generic CPU", max_len - 1);
    }
}

static void get_mem_info(char *total_buf, size_t t_len, char *avail_buf, size_t a_len) {
    total_buf[0] = '\0';
    avail_buf[0] = '\0';
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            char *p = line + 9;
            while (*p == ' ') p++;
            size_t l = strlen(p);
            while (l > 0 && (p[l - 1] == '\n' || p[l - 1] == '\r')) p[--l] = '\0';
            snprintf(total_buf, t_len, "%s", p);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            char *p = line + 13;
            while (*p == ' ') p++;
            size_t l = strlen(p);
            while (l > 0 && (p[l - 1] == '\n' || p[l - 1] == '\r')) p[--l] = '\0';
            snprintf(avail_buf, a_len, "%s", p);
        }
    }
    fclose(f);
}

int main(void) {
    printf("=================================================================\n");
    printf("     Linux System Telemetry & Multi-Threaded Benchmark\n");
    printf("=================================================================\n\n");

    // 1. Hardware & OS Probe
    char cpu_model[128] = {0};
    get_cpu_model(cpu_model, sizeof(cpu_model));

    char mem_total[64] = "N/A", mem_avail[64] = "N/A";
    get_mem_info(mem_total, sizeof(mem_total), mem_avail, sizeof(mem_avail));

    char kernel_ver[256] = {0};
    read_first_line("/proc/version", kernel_ver, sizeof(kernel_ver));
    if (kernel_ver[0] == '\0') strncpy(kernel_ver, "POSIX System", sizeof(kernel_ver) - 1);

    char loadavg[64] = {0};
    read_first_line("/proc/loadavg", loadavg, sizeof(loadavg));
    if (loadavg[0] == '\0') strncpy(loadavg, "0.00 0.00 0.00", sizeof(loadavg) - 1);

    long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
    if (nprocs < 1) nprocs = 1;

    printf("[System Information]\n");
    printf("  • CPU Model:       %s\n", cpu_model);
    printf("  • Online Cores:    %ld\n", nprocs);
    printf("  • Memory:          %s total / %s available\n", mem_total, mem_avail);
    printf("  • System Load:     %s\n", loadavg);
    printf("  • Kernel:          %.70s...\n\n", kernel_ver);

    // 2. Multi-threaded Compute Benchmark
    printf("[Compute Benchmark]\n");
    printf("  • Workload:        Computing primes up to %d across %d threads\n", PRIME_LIMIT, NUM_BENCH_THREADS);

    struct timespec bench_start, bench_end;
    clock_gettime(CLOCK_MONOTONIC, &bench_start);

    pthread_t threads[NUM_BENCH_THREADS];
    ThreadResult results[NUM_BENCH_THREADS];

    for (int i = 0; i < NUM_BENCH_THREADS; i++) {
        results[i].thread_id = i;
        results[i].count = 0;
        results[i].elapsed_ms = 0;
        pthread_create(&threads[i], NULL, worker_func, &results[i]);
    }

    for (int i = 0; i < NUM_BENCH_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    clock_gettime(CLOCK_MONOTONIC, &bench_end);
    double total_wall_ms = ((double)(bench_end.tv_sec - bench_start.tv_sec) * 1000.0) +
                           ((double)(bench_end.tv_nsec - bench_start.tv_nsec) / 1e6);

    uint64_t total_primes_found = 0;
    for (int i = 0; i < NUM_BENCH_THREADS; i++) {
        total_primes_found += results[i].count;
        printf("    - Thread #%d: found %llu primes in %.2f ms\n",
               i, (unsigned long long)results[i].count, results[i].elapsed_ms);
    }

    double ops_per_sec = ((double)PRIME_LIMIT * NUM_BENCH_THREADS) / (total_wall_ms / 1000.0);
    double mops = ops_per_sec / 1e6;

    printf("\n[Benchmark Summary]\n");
    printf("  • Primes Found:    %llu\n", (unsigned long long)total_primes_found);
    printf("  • Wall Time:       %.2f ms\n", total_wall_ms);
    printf("  • Compute Rate:    %.2f Mops/s\n", mops);
    printf("  • Status:          SUCCESS\n\n");

    // 3. Write structured JSON artifact
    FILE *json_out = fopen("benchmark_report.json", "w");
    if (json_out) {
        fprintf(json_out, "{\n");
        fprintf(json_out, "  \"cpu_model\": \"%s\",\n", cpu_model);
        fprintf(json_out, "  \"cores_online\": %ld,\n", nprocs);
        fprintf(json_out, "  \"memory_total\": \"%s\",\n", mem_total);
        fprintf(json_out, "  \"memory_available\": \"%s\",\n", mem_avail);
        fprintf(json_out, "  \"system_load\": \"%s\",\n", loadavg);
        fprintf(json_out, "  \"threads_benchmarked\": %d,\n", NUM_BENCH_THREADS);
        fprintf(json_out, "  \"wall_time_ms\": %.2f,\n", total_wall_ms);
        fprintf(json_out, "  \"throughput_mops\": %.2f,\n", mops);
        fprintf(json_out, "  \"timestamp\": %ld\n", (long)time(NULL));
        fprintf(json_out, "}\n");
        fclose(json_out);
        printf("[✓] Generated artifact: 'benchmark_report.json'\n");
    }

    printf("=================================================================\n");
    return 0;
}
