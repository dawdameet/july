#include "hash_table.h"
#include <string.h>

static uint32_t fnv1a(const char *str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

void ht_init(SimpleHt *ht) {
    memset(ht, 0, sizeof(*ht));
}

int ht_put(SimpleHt *ht, const char *key, uint64_t value) {
    uint32_t idx = fnv1a(key) % HT_CAPACITY;
    for (size_t i = 0; i < HT_CAPACITY; i++) {
        size_t slot = (idx + i) % HT_CAPACITY;
        if (!ht->entries[slot].occupied || strcmp(ht->entries[slot].key, key) == 0) {
            strncpy(ht->entries[slot].key, key, sizeof(ht->entries[slot].key) - 1);
            ht->entries[slot].value = value;
            if (!ht->entries[slot].occupied) {
                ht->entries[slot].occupied = 1;
                ht->count++;
            }
            return 1;
        }
    }
    return 0;
}

int ht_get(SimpleHt *ht, const char *key, uint64_t *out_val) {
    uint32_t idx = fnv1a(key) % HT_CAPACITY;
    for (size_t i = 0; i < HT_CAPACITY; i++) {
        size_t slot = (idx + i) % HT_CAPACITY;
        if (!ht->entries[slot].occupied) return 0;
        if (strcmp(ht->entries[slot].key, key) == 0) {
            if (out_val) *out_val = ht->entries[slot].value;
            return 1;
        }
    }
    return 0;
}

