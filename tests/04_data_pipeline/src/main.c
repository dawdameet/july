#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define TOTAL_TICKS 250000

#pragma pack(push, 1)
typedef struct {
    uint64_t timestamp_ns;
    char symbol[8];
    double price;
    uint32_t volume;
    uint8_t side; // 1 = Buy, 2 = Sell
} MarketTick;
#pragma pack(pop)

static const char *SYMBOLS[] = { "AAPL", "GOOG", "NVDA", "TSLA" };
static const double BASE_PRICES[] = { 230.50, 185.20, 142.80, 260.10 };

int main(void)
{
    printf("=================================================================\n");
    printf("     High-Frequency Market Tick Stream Generator (C Engine)      \n");
    printf("=================================================================\n\n");
    printf("[Config] Emitting %d binary market ticks to 'ticks.bin'...\n", TOTAL_TICKS);

    const char *out_path = "ticks.bin";
    FILE *f = fopen(out_path, "wb");
    if (!f) {
        perror("fopen ticks.bin");
        return 1;
    }

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t base_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    uint32_t magic = 0x5449434B; // "TICK"
    uint32_t count = TOTAL_TICKS;
    uint32_t tick_size = sizeof(MarketTick);

    fwrite(&magic, sizeof(uint32_t), 1, f);
    fwrite(&count, sizeof(uint32_t), 1, f);
    fwrite(&tick_size, sizeof(uint32_t), 1, f);

    MarketTick *buffer = (MarketTick *)malloc(sizeof(MarketTick) * 5000);
    int buf_idx = 0;

    unsigned int seed = 42;
    for (int i = 0; i < TOTAL_TICKS; i++) {
        int sym_idx = i % 4;
        MarketTick *t = &buffer[buf_idx++];
        t->timestamp_ns = base_ns + (uint64_t)i * 1000ULL; // +1 microsecond per tick
        memset(t->symbol, 0, sizeof(t->symbol));
        strncpy(t->symbol, SYMBOLS[sym_idx], sizeof(t->symbol) - 1);
        
        // Pseudo-random price fluctuation
        double delta = ((double)(rand_r(&seed) % 2000) - 1000.0) / 100.0;
        t->price = BASE_PRICES[sym_idx] + delta;
        t->volume = (rand_r(&seed) % 500) + 1;
        t->side = (rand_r(&seed) % 2) ? 1 : 2;

        if (buf_idx == 5000) {
            fwrite(buffer, sizeof(MarketTick), 5000, f);
            buf_idx = 0;
        }
    }
    if (buf_idx > 0) {
        fwrite(buffer, sizeof(MarketTick), buf_idx, f);
    }

    fclose(f);
    free(buffer);

    size_t total_bytes = sizeof(uint32_t) * 3 + (size_t)TOTAL_TICKS * sizeof(MarketTick);
    printf("[✓] Successfully wrote %zu bytes (%d ticks) to '%s'\n", total_bytes, TOTAL_TICKS, out_path);
    return 0;
}
