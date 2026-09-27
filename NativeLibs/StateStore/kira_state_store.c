#include "kira_state_store.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern uint32_t kira_rt_native_state_release(uint64_t token);

static void release_value(void* value) {
    if (value == NULL) return;
    (void)kira_rt_native_state_release((uint64_t)(uintptr_t)value);
}

typedef struct kira_state_entry {
    uint64_t scope;
    uint64_t hash;
    char* key;
    void* value;
    struct kira_state_entry* next;
} kira_state_entry;

static kira_state_entry** buckets = NULL;
static size_t bucket_count = 0;
static int32_t entry_count = 0;
static uint64_t current_scope = 0;
static uint64_t next_scope = 1;

static uint64_t hash_key(uint64_t scope, const char* key) {
    uint64_t hash = UINT64_C(1469598103934665603) ^ scope;
    for (const unsigned char* p = (const unsigned char*)key; *p != 0; ++p) {
        hash ^= (uint64_t)*p;
        hash *= UINT64_C(1099511628211);
    }
    // Keep zero available as an unmistakable uninitialized value while preserving
    // the hash's distribution for practical keys.
    return hash == 0 ? UINT64_C(0x9e3779b97f4a7c15) : hash;
}

static int ensure_buckets(void) {
    if (bucket_count != 0) return 1;
    bucket_count = 64;
    buckets = calloc(bucket_count, sizeof(kira_state_entry*));
    if (buckets == NULL) {
        bucket_count = 0;
        return 0;
    }
    return 1;
}

static int rehash(size_t next_count) {
    kira_state_entry** next = calloc(next_count, sizeof(kira_state_entry*));
    if (next == NULL) return 0;
    for (size_t i = 0; i < bucket_count; ++i) {
        kira_state_entry* entry = buckets[i];
        while (entry != NULL) {
            kira_state_entry* following = entry->next;
            const size_t bucket = (size_t)(entry->hash % next_count);
            entry->next = next[bucket];
            next[bucket] = entry;
            entry = following;
        }
    }
    free(buckets);
    buckets = next;
    bucket_count = next_count;
    return 1;
}

static kira_state_entry* find_entry(uint64_t scope, const char* key, uint64_t hash) {
    if (key == NULL || bucket_count == 0) return NULL;
    const size_t bucket = (size_t)(hash % bucket_count);
    for (kira_state_entry* entry = buckets[bucket]; entry != NULL; entry = entry->next) {
        if (entry->scope == scope && entry->hash == hash && strcmp(entry->key, key) == 0) {
            return entry;
        }
    }
    return NULL;
}

uint64_t kira_state_scope_create(void) {
    const uint64_t scope = next_scope;
    next_scope++;
    if (next_scope == 0) next_scope = 1;
    return scope;
}

uint64_t kira_state_scope_current(void) { return current_scope; }
void kira_state_scope_enter(uint64_t scope) { current_scope = scope; }

int32_t kira_state_slot_has(const char* key) {
    if (key == NULL) return 0;
    const uint64_t hash = hash_key(current_scope, key);
    return find_entry(current_scope, key, hash) != NULL ? 1 : 0;
}

void* kira_state_slot_get(const char* key) {
    if (key == NULL) return NULL;
    const uint64_t hash = hash_key(current_scope, key);
    kira_state_entry* entry = find_entry(current_scope, key, hash);
    return entry == NULL ? NULL : entry->value;
}

void kira_state_slot_put(const char* key, void* value) {
    if (key == NULL) {
        release_value(value);
        return;
    }
    if (!ensure_buckets()) {
        release_value(value);
        return;
    }
    const uint64_t hash = hash_key(current_scope, key);
    kira_state_entry* existing = find_entry(current_scope, key, hash);
    if (existing != NULL) {
        release_value(existing->value);
        existing->value = value;
        return;
    }
    if ((size_t)(entry_count + 1) * 4 >= bucket_count * 3) {
        (void)rehash(bucket_count * 2);
    }
    kira_state_entry* entry = malloc(sizeof(kira_state_entry));
    if (entry == NULL) {
        release_value(value);
        return;
    }
    char* owned_key = strdup(key);
    if (owned_key == NULL) {
        free(entry);
        release_value(value);
        return;
    }
    entry->scope = current_scope;
    entry->hash = hash;
    entry->key = owned_key;
    entry->value = value;
    const size_t bucket = (size_t)(hash % bucket_count);
    entry->next = buckets[bucket];
    buckets[bucket] = entry;
    entry_count++;
}

int32_t kira_state_slot_remove(const char* key) {
    if (key == NULL || bucket_count == 0) return 0;
    const uint64_t hash = hash_key(current_scope, key);
    const size_t bucket = (size_t)(hash % bucket_count);
    kira_state_entry** cursor = &buckets[bucket];
    while (*cursor != NULL) {
        kira_state_entry* entry = *cursor;
        if (entry->scope == current_scope && entry->hash == hash && strcmp(entry->key, key) == 0) {
            *cursor = entry->next;
            release_value(entry->value);
            free(entry->key);
            free(entry);
            entry_count--;
            return 1;
        }
        cursor = &entry->next;
    }
    return 0;
}

static int32_t reset_scope(uint64_t scope) {
    if (bucket_count == 0) return 0;
    int32_t removed = 0;
    for (size_t i = 0; i < bucket_count; ++i) {
        kira_state_entry** cursor = &buckets[i];
        while (*cursor != NULL) {
            kira_state_entry* entry = *cursor;
            if (entry->scope == scope) {
                *cursor = entry->next;
                release_value(entry->value);
                free(entry->key);
                free(entry);
                entry_count--;
                removed++;
            } else {
                cursor = &entry->next;
            }
        }
    }
    return removed;
}

void kira_state_slot_reset(void) { (void)reset_scope(current_scope); }

void kira_state_slot_reset_all(void) {
    if (bucket_count == 0) return;
    for (size_t i = 0; i < bucket_count; ++i) {
        kira_state_entry* entry = buckets[i];
        while (entry != NULL) {
            kira_state_entry* following = entry->next;
            release_value(entry->value);
            free(entry->key);
            free(entry);
            entry = following;
        }
    }
    free(buckets);
    buckets = NULL;
    bucket_count = 0;
    entry_count = 0;
}

int32_t kira_state_slot_count(void) {
    if (bucket_count == 0) return 0;
    int32_t count = 0;
    for (size_t i = 0; i < bucket_count; ++i) {
        for (kira_state_entry* entry = buckets[i]; entry != NULL; entry = entry->next) {
            if (entry->scope == current_scope) count++;
        }
    }
    return count;
}

int32_t kira_state_slot_count_all(void) { return entry_count; }
