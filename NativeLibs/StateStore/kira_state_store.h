// Scoped ambient key -> native-state slots for UI state that survives widget rebuilds.
//
// Each Foundation runner enters its own scope before starting its event loop. Slot
// operations address only the current scope, so two Foundation instances in one
// process cannot overwrite or reset each other's input/geometry/scroll state.
// Values are one retained Kira NativeState reference each; replacement, removal,
// scope reset, and global reset release exactly that reference.
//
// Single-threaded by contract (the UI frame loop); no locking.

#ifndef KIRA_STATE_STORE_H
#define KIRA_STATE_STORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint64_t kira_state_scope_create(void);
uint64_t kira_state_scope_current(void);
void kira_state_scope_enter(uint64_t scope);

int32_t kira_state_slot_has(const char* key);
void* kira_state_slot_get(const char* key);
void kira_state_slot_put(const char* key, void* value);
int32_t kira_state_slot_remove(const char* key);

// Clear only the currently-entered scope.
void kira_state_slot_reset(void);
// Clear every scope. Intended for process/test teardown only.
void kira_state_slot_reset_all(void);

int32_t kira_state_slot_count(void);
int32_t kira_state_slot_count_all(void);

#ifdef __cplusplus
}
#endif

#endif
