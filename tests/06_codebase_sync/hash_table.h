#ifndef HASH_TABLE_H
#define HASH_TABLE_H

#include <stddef.h>
#include <stdint.h>

#define HT_CAPACITY 256

typedef struct {
    char key[64];
    uint64_t value;
    int occupied;
} HtEntry;

typedef struct {
    HtEntry entries[HT_CAPACITY];
    size_t count;
} SimpleHt;

void ht_init(SimpleHt *ht);
int ht_put(SimpleHt *ht, const char *key, uint64_t value);
int ht_get(SimpleHt *ht, const char *key, uint64_t *out_val);

#endif

