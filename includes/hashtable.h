#ifndef WMS_HASHTABLE_H
#define WMS_HASHTABLE_H

#include <stddef.h>

/*
 * Minimal string-keyed hash table, open addressing + linear probing.
 * Values are `void*` (we point them at Product structs owned by the
 * inventory module's product array - this table never owns memory).
 *
 * Why not just use SQLite for the search box? A DB round trip (even to
 * SQLite, even indexed) is milliseconds and involves parsing SQL on every
 * keystroke. A hash lookup here is tens of nanoseconds. For a catalog
 * that fits in RAM (tens of thousands of SKUs) this is the difference
 * between an instant-feeling search box and a laggy one.
 *
 * NOTE on scaling: past ~100k+ SKUs with heavy prefix-typing search,
 * swap this for a trie (prefix tree) - O(k) per lookup where k = query
 * length, and naturally supports "starts with" without a linear scan.
 */
typedef struct {
    char  *key;     /* NULL = empty slot, (char*)-1 = tombstone (deleted) */
    void  *value;
} HashEntry;

typedef struct {
    HashEntry *entries;
    size_t     capacity;   /* always a power of two */
    size_t     count;
} HashTable;

void  ht_init(HashTable *ht, size_t initial_capacity);
void  ht_free(HashTable *ht);
void  ht_put(HashTable *ht, const char *key, void *value); /* overwrites existing */
void *ht_get(HashTable *ht, const char *key);               /* NULL if absent */
void  ht_remove(HashTable *ht, const char *key);
void  ht_clear(HashTable *ht);

#endif
