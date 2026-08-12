#define _DEFAULT_SOURCE
#include "hashtable.h"
#include <stdlib.h>
#include <string.h>

#define TOMBSTONE ((char *)-1)

static unsigned long fnv1a(const char *s) {
    unsigned long h = 2166136261UL;
    while (*s) {
        h ^= (unsigned char)(*s++);
        h *= 16777619UL;
    }
    return h;
}

static size_t next_pow2(size_t n) {
    size_t p = 16;
    while (p < n) p <<= 1;
    return p;
}

void ht_init(HashTable *ht, size_t initial_capacity) {
    ht->capacity = next_pow2(initial_capacity ? initial_capacity : 16);
    ht->entries  = calloc(ht->capacity, sizeof(HashEntry));
    ht->count    = 0;
}

void ht_free(HashTable *ht) {
    for (size_t i = 0; i < ht->capacity; i++) {
        if (ht->entries[i].key && ht->entries[i].key != TOMBSTONE)
            free(ht->entries[i].key);
    }
    free(ht->entries);
    ht->entries = NULL;
    ht->capacity = ht->count = 0;
}

void ht_clear(HashTable *ht) {
    size_t cap = ht->capacity;
    ht_free(ht);
    ht_init(ht, cap);
}

static void ht_resize(HashTable *ht, size_t new_capacity) {
    HashEntry *old = ht->entries;
    size_t old_cap = ht->capacity;

    ht->entries  = calloc(new_capacity, sizeof(HashEntry));
    ht->capacity = new_capacity;
    ht->count    = 0;

    for (size_t i = 0; i < old_cap; i++) {
        if (old[i].key && old[i].key != TOMBSTONE) {
            ht_put(ht, old[i].key, old[i].value);
            free(old[i].key);
        }
    }
    free(old);
}

void ht_put(HashTable *ht, const char *key, void *value) {
    if ((double)(ht->count + 1) / (double)ht->capacity > 0.7)
        ht_resize(ht, ht->capacity * 2);

    size_t mask = ht->capacity - 1;
    size_t idx  = fnv1a(key) & mask;

    for (;;) {
        HashEntry *e = &ht->entries[idx];
        if (e->key == NULL || e->key == TOMBSTONE) {
            e->key = strdup(key);
            e->value = value;
            ht->count++;
            return;
        }
        if (strcmp(e->key, key) == 0) {
            e->value = value; /* overwrite */
            return;
        }
        idx = (idx + 1) & mask;
    }
}

void *ht_get(HashTable *ht, const char *key) {
    if (ht->capacity == 0) return NULL;
    size_t mask = ht->capacity - 1;
    size_t idx  = fnv1a(key) & mask;
    size_t start = idx;

    for (;;) {
        HashEntry *e = &ht->entries[idx];
        if (e->key == NULL) return NULL; /* empty slot = not found */
        if (e->key != TOMBSTONE && strcmp(e->key, key) == 0) return e->value;
        idx = (idx + 1) & mask;
        if (idx == start) return NULL; /* fully wrapped, table full of tombstones */
    }
}

void ht_remove(HashTable *ht, const char *key) {
    if (ht->capacity == 0) return;
    size_t mask = ht->capacity - 1;
    size_t idx  = fnv1a(key) & mask;
    size_t start = idx;

    for (;;) {
        HashEntry *e = &ht->entries[idx];
        if (e->key == NULL) return;
        if (e->key != TOMBSTONE && strcmp(e->key, key) == 0) {
            free(e->key);
            e->key = TOMBSTONE;
            ht->count--;
            return;
        }
        idx = (idx + 1) & mask;
        if (idx == start) return;
    }
}
