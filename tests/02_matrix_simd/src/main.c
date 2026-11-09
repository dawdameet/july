#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>

#define DEFAULT_DIM 512
#define TILE_SIZE 32

typedef struct {
    int thread_id;
    int num_threads;
    int n;
    const float *A;
    const float *B;
    float *C;
    double thread_time_ms;
} WorkerArg;

static double get_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static void *matrix_multiply_worker(void *arg)
{
    WorkerArg *w = (WorkerArg *)arg;
    int n = w->n;
    const float *A = w->A;
    const float *B = w->B;
    float *C = w->C;

    double start = get_time_ms();

    // Divide rows among threads
    int rows_per_thread = (n + w->num_threads - 1) / w->num_threads;
    int r_start = w->thread_id * rows_per_thread;
    int r_end = r_start + rows_per_thread;
    if (r_end > n) r_end = n;

    // Cache-tiled matrix multiplication for locality
    for (int sj = 0; sj < n; sj += TILE_SIZE) {
        for (int sk = 0; sk < n; sk += TILE_SIZE) {
            for (int i = r_start; i < r_end; i++) {
                for (int k = sk; k < sk + TILE_SIZE && k < n; k++) {
                    float a_ik = A[i * n + k];
                    for (int j = sj; j < sj + TILE_SIZE && j < n; j++) {
                        C[i * n + j] += a_ik * B[k * n + j];
                    }
                }
            }
        }
    }

    w->thread_time_ms = get_time_ms() - start;
    return NULL;
}

int main(int argc, char **argv)
{
    int n = DEFAULT_DIM;
    if (argc > 1) {
        n = atoi(argv[1]);
        if (n <= 0 || n > 2048) n = DEFAULT_DIM;
    }

    int num_cores = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (num_cores < 1) num_cores = 1;

    printf("=================================================================\n");
    printf("     High-Performance Matrix Multiply & Cache Locality Test     \n");
    printf("=================================================================\n\n");
    printf("[Config] Matrix: %d x %d | Tile Size: %d | Worker Threads: %d\n", n, n, TILE_SIZE, num_cores);

    size_t matrix_bytes = (size_t)n * n * sizeof(float);
    float *A = (float *)malloc(matrix_bytes);
    float *B = (float *)malloc(matrix_bytes);
    float *C = (float *)calloc(n * n, sizeof(float));

    if (!A || !B || !C) {
        fprintf(stderr, "Error: Failed to allocate matrix memory (%zu bytes each)\n", matrix_bytes);
        return 1;
    }

    // Initialize matrices A and B with deterministic values
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < n; j++) {
            A[i * n + j] = (float)((i + j) % 17) * 0.1f;
            B[i * n + j] = (float)((i * 3 + j) % 19) * 0.1f;
        }
    }

    pthread_t *threads = (pthread_t *)malloc(sizeof(pthread_t) * num_cores);
    WorkerArg *args = (WorkerArg *)malloc(sizeof(WorkerArg) * num_cores);

    printf("[Execution] Launching parallel block-tiled computation...\n");
    double total_start = get_time_ms();

    for (int t = 0; t < num_cores; t++) {
        args[t].thread_id = t;
        args[t].num_threads = num_cores;
        args[t].n = n;
        args[t].A = A;
        args[t].B = B;
        args[t].C = C;
        args[t].thread_time_ms = 0.0;
        pthread_create(&threads[t], NULL, matrix_multiply_worker, &args[t]);
    }

    for (int t = 0; t < num_cores; t++) {
        pthread_join(threads[t], NULL);
        printf("  • Thread #%d: completed in %.2f ms\n", t, args[t].thread_time_ms);
    }

    double total_elapsed = get_time_ms() - total_start;

    // Total floating point operations: 2 * N^3 (multiplications + additions)
    double total_ops = 2.0 * (double)n * (double)n * (double)n;
    double gflops = (total_ops / (total_elapsed / 1000.0)) / 1e9;

    // Compute Frobenius norm for verification
    double frobenius_sq = 0.0;
    for (int i = 0; i < n * n; i++) {
        frobenius_sq += (double)C[i] * (double)C[i];
    }
    double frobenius_norm = sqrt(frobenius_sq);

    printf("\n[Benchmark Summary]\n");
    printf("  • Wall Time:       %.2f ms\n", total_elapsed);
    printf("  • Total Work:      %.2f Gflop (2 * %d^3 ops)\n", total_ops / 1e9, n);
    printf("  • Compute Rate:    %.2f GFLOPS\n", gflops);
    printf("  • Frobenius Norm:  %.4f\n", frobenius_norm);
    printf("  • Status:          SUCCESS\n\n");

    // Write binary matrix artifact (Header: "MATX", rows, cols, data)
    const char *bin_path = "matrix_result.bin";
    FILE *bf = fopen(bin_path, "wb");
    if (bf) {
        uint32_t header[4] = {
            0x5854414D, // "MATX" in LE
            (uint32_t)n,
            (uint32_t)n,
            (uint32_t)sizeof(float)
        };
        fwrite(header, sizeof(uint32_t), 4, bf);
        // Write the top-left 64x64 submatrix or full matrix if small to keep size compact
        int export_dim = n > 128 ? 128 : n;
        for (int i = 0; i < export_dim; i++) {
            fwrite(&C[i * n], sizeof(float), export_dim, bf);
        }
        fclose(bf);
        printf("[✓] Exported binary matrix artifact: '%s'\n", bin_path);
    }

    // Write JSON performance report
    const char *json_path = "matrix_report.json";
    FILE *jf = fopen(json_path, "w");
    if (jf) {
        fprintf(jf, "{\n");
        fprintf(jf, "  \"application\": \"matrix_simd\",\n");
        fprintf(jf, "  \"dimension\": %d,\n", n);
        fprintf(jf, "  \"threads\": %d,\n", num_cores);
        fprintf(jf, "  \"tile_size\": %d,\n", TILE_SIZE);
        fprintf(jf, "  \"wall_time_ms\": %.2f,\n", total_elapsed);
        fprintf(jf, "  \"gflops\": %.2f,\n", gflops);
        fprintf(jf, "  \"frobenius_norm\": %.4f,\n", frobenius_norm);
        fprintf(jf, "  \"timestamp\": %ld\n", time(NULL));
        fprintf(jf, "}\n");
        fclose(jf);
        printf("[✓] Exported JSON telemetry report: '%s'\n", json_path);
    }

    free(threads);
    free(args);
    free(A);
    free(B);
    free(C);
    return 0;
}
