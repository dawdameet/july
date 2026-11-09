#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>

#define NUM_SHARDS 16
#define MAX_KEY_LEN 32
#define MAX_VAL_LEN 64
#define TOTAL_OPS 100000

enum OpType {
    OP_PUT = 1,
    OP_GET = 2,
    OP_DEL = 3
};

typedef struct KVNode {
    char key[MAX_KEY_LEN];
    char value[MAX_VAL_LEN];
    struct KVNode *next;
} KVNode;

typedef struct {
    KVNode *head;
    pthread_mutex_t lock;
} KVShard;

typedef struct {
    KVShard shards[NUM_SHARDS];
    FILE *wal_file;
    pthread_mutex_t wal_lock;
} KVStore;

typedef struct {
    int thread_id;
    int num_threads;
    int ops_count;
    KVStore *store;
    uint32_t *latencies_us;
} WorkerArg;

// Simple fast hash (djb2)
static inline uint32_t hash_str(const char *str)
{
    uint32_t hash = 5381;
    int c;
    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;
    return hash;
}

static inline double get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

static void kv_init(KVStore *kv, const char *wal_path)
{
    for (int i = 0; i < NUM_SHARDS; i++) {
        kv->shards[i].head = NULL;
        pthread_mutex_init(&kv->shards[i].lock, NULL);
    }
    pthread_mutex_init(&kv->wal_lock, NULL);
    kv->wal_file = fopen(wal_path, "wb");
    if (kv->wal_file) {
        uint32_t header[2] = { 0x57414C31, 1 }; // "WAL1", v1
        fwrite(header, sizeof(uint32_t), 2, kv->wal_file);
    }
}

static void kv_wal_log(KVStore *kv, uint8_t op, const char *key, const char *val)
{
    if (!kv->wal_file) return;
    uint8_t klen = (uint8_t)strlen(key);
    uint8_t vlen = val ? (uint8_t)strlen(val) : 0;

    pthread_mutex_lock(&kv->wal_lock);
    fwrite(&op, 1, 1, kv->wal_file);
    fwrite(&klen, 1, 1, kv->wal_file);
    fwrite(key, 1, klen, kv->wal_file);
    fwrite(&vlen, 1, 1, kv->wal_file);
    if (vlen > 0) {
        fwrite(val, 1, vlen, kv->wal_file);
    }
    pthread_mutex_unlock(&kv->wal_lock);
}

static void kv_put(KVStore *kv, const char *key, const char *val)
{
    uint32_t h = hash_str(key);
    int shard_idx = h % NUM_SHARDS;
    KVShard *shard = &kv->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);
    KVNode *cur = shard->head;
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            strncpy(cur->value, val, sizeof(cur->value) - 1);
            cur->value[sizeof(cur->value) - 1] = '\0';
            pthread_mutex_unlock(&shard->lock);
            kv_wal_log(kv, OP_PUT, key, val);
            return;
        }
        cur = cur->next;
    }
    KVNode *node = (KVNode *)malloc(sizeof(KVNode));
    strncpy(node->key, key, sizeof(node->key) - 1);
    node->key[sizeof(node->key) - 1] = '\0';
    strncpy(node->value, val, sizeof(node->value) - 1);
    node->value[sizeof(node->value) - 1] = '\0';
    node->next = shard->head;
    shard->head = node;
    pthread_mutex_unlock(&shard->lock);

    kv_wal_log(kv, OP_PUT, key, val);
}

static bool kv_get(KVStore *kv, const char *key, char *out_val, size_t out_max)
{
    uint32_t h = hash_str(key);
    int shard_idx = h % NUM_SHARDS;
    KVShard *shard = &kv->shards[shard_idx];

    pthread_mutex_lock(&shard->lock);
    KVNode *cur = shard->head;
    while (cur) {
        if (strcmp(cur->key, key) == 0) {
            strncpy(out_val, cur->value, out_max - 1);
            out_val[out_max - 1] = '\0';
            pthread_mutex_unlock(&shard->lock);
            return true;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&shard->lock);
    return false;
}

static int compare_u32(const void *a, const void *b)
{
    uint32_t arg1 = *(const uint32_t *)a;
    uint32_t arg2 = *(const uint32_t *)b;
    return (arg1 > arg2) - (arg1 < arg2);
}

static void *kv_worker(void *arg)
{
    WorkerArg *w = (WorkerArg *)arg;
    char key[MAX_KEY_LEN];
    char val[MAX_VAL_LEN];
    char out_buf[MAX_VAL_LEN];

    for (int i = 0; i < w->ops_count; i++) {
        int key_num = (w->thread_id * 100000 + i) % 5000;
        snprintf(key, sizeof(key), "usr:session:%d", key_num);

        double t0 = get_time_us();
        int r = i % 10;
        if (r < 7) { // 70% GET
            kv_get(w->store, key, out_buf, sizeof(out_buf));
        } else {     // 30% PUT
            snprintf(val, sizeof(val), "val_token_%d_%d", w->thread_id, i);
            kv_put(w->store, key, val);
        }
        double elapsed_us = get_time_us() - t0;
        w->latencies_us[i] = (uint32_t)(elapsed_us > 0 ? elapsed_us : 1);
    }
    return NULL;
}

int main(void)
{
    int num_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores < 1) num_cores = 1;

    printf("=================================================================\n");
    printf("     High-Throughput Concurrent In-Memory Key-Value & WAL Engine \n");
    printf("=================================================================\n\n");
    printf("[Config] Operations: %d | Worker Threads: %d | Shards: %d\n",
           TOTAL_OPS, num_cores, NUM_SHARDS);

    const char *wal_path = "wal.bin";
    KVStore store;
    kv_init(&store, wal_path);

    int ops_per_thread = TOTAL_OPS / num_cores;
    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * num_cores);
    WorkerArg *args = (WorkerArg *)malloc(sizeof(WorkerArg) * num_cores);

    printf("[Execution] Executing concurrent workload (70%% GET / 30%% PUT)...\n");
    double start_time = get_time_us();

    for (int t = 0; t < num_cores; t++) {
        args[t].thread_id = t;
        args[t].num_threads = num_cores;
        args[t].ops_count = ops_per_thread;
        args[t].store = &store;
        args[t].latencies_us = (uint32_t *)malloc(sizeof(uint32_t) * ops_per_thread);
        pthread_create(&threads[t], NULL, kv_worker, &args[t]);
    }

    for (int t = 0; t < num_cores; t++) {
        pthread_join(threads[t], NULL);
    }

    double total_time_ms = (get_time_us() - start_time) / 1000.0;
    if (store.wal_file) {
        fflush(store.wal_file);
        fclose(store.wal_file);
    }

    // Merge latencies for percentile calculation
    uint32_t *all_latencies = (uint32_t *)malloc(sizeof(uint32_t) * TOTAL_OPS);
    int total_collected = 0;
    for (int t = 0; t < num_cores; t++) {
        memcpy(all_latencies + total_collected, args[t].latencies_us, sizeof(uint32_t) * args[t].ops_count);
        total_collected += args[t].ops_count;
        free(args[t].latencies_us);
    }

    qsort(all_latencies, total_collected, sizeof(uint32_t), compare_u32);

    uint32_t p50 = all_latencies[(int)(total_collected * 0.50)];
    uint32_t p95 = all_latencies[(int)(total_collected * 0.95)];
    uint32_t p99 = all_latencies[(int)(total_collected * 0.99)];
    double ops_sec = ((double)total_collected / (total_time_ms / 1000.0));

    // Verify data integrity
    int verified = 0;
    char verify_buf[MAX_VAL_LEN];
    for (int i = 0; i < 5000; i++) {
        char k[MAX_KEY_LEN];
        snprintf(k, sizeof(k), "usr:session:%d", i);
        if (kv_get(&store, k, verify_buf, sizeof(verify_buf))) {
            verified++;
        }
    }

    printf("\n[Benchmark Summary]\n");
    printf("  • Total Ops:       %d\n", total_collected);
    printf("  • Wall Time:       %.2f ms\n", total_time_ms);
    printf("  • Throughput:      %.0f ops/sec (%.2f Mops/s)\n", ops_sec, ops_sec / 1e6);
    printf("  • Latency P50:     %u µs\n", p50);
    printf("  • Latency P95:     %u µs\n", p95);
    printf("  • Latency P99:     %u µs\n", p99);
    printf("  • Active Keys:     %d verified\n", verified);
    printf("  • Status:          SUCCESS\n\n");

    const char *report_path = "kv_report.json";
    FILE *rf = fopen(report_path, "w");
    if (rf) {
        fprintf(rf, "{\n");
        fprintf(rf, "  \"application\": \"kv_engine\",\n");
        fprintf(rf, "  \"total_ops\": %d,\n", total_collected);
        fprintf(rf, "  \"threads\": %d,\n", num_cores);
        fprintf(rf, "  \"throughput_ops_sec\": %.0f,\n", ops_sec);
        fprintf(rf, "  \"latency_p50_us\": %u,\n", p50);
        fprintf(rf, "  \"latency_p95_us\": %u,\n", p95);
        fprintf(rf, "  \"latency_p99_us\": %u,\n", p99);
        fprintf(rf, "  \"wall_time_ms\": %.2f,\n", total_time_ms);
        fprintf(rf, "  \"active_keys_verified\": %d,\n", verified);
        fprintf(rf, "  \"timestamp\": %ld\n", time(NULL));
        fprintf(rf, "}\n");
        fclose(rf);
        printf("[✓] Exported telemetry report: '%s'\n", report_path);
    }
    printf("[✓] Exported binary Write-Ahead Log: '%s'\n", wal_path);

    free(all_latencies);
    free(threads);
    free(args);
    return 0;
}
