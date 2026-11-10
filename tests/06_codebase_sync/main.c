#include <stdio.h>
#include <stdlib.h>
#include "math_engine.h"
#include "string_parser.h"
#include "hash_table.h"
#include "telemetry.h"

int main(void) {
    uint64_t t0 = get_time_ns();

    // 1. Math check
    double a[4] = {1.0, 2.0, 3.0, 4.0};
    double b[4] = {2.0, 3.0, 4.0, 5.0};
    double dot = vector_dot(a, b, 4);

    // 2. Parser check
    char msg[64] = "   fast ambient compute engine   ";
    char *trimmed = trim_whitespace(msg);
    char *words[8];
    size_t nw = split_words(trimmed, words, 8);

    // 3. Hash table check
    SimpleHt ht;
    ht_init(&ht);
    ht_put(&ht, "alpha", 42);
    ht_put(&ht, "beta", 1337);
    uint64_t v = 0;
    ht_get(&ht, "beta", &v);

    uint64_t t1 = get_time_ns();
    printf("SyncApp OK | dot=%.1f | words=%zu | ht_val=%llu | runtime=%llu ns\n",
           dot, nw, (unsigned long long)v, (unsigned long long)(t1 - t0));
    return 0;
}

