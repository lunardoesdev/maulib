#ifndef MILIB_BRIDGE_H
#define MILIB_BRIDGE_H

#include <stdint.h>
#include <stdlib.h>

/*
 * Callback trampoline.
 *
 * Go code must not call a C function pointer directly, so every task callback
 * is dispatched through milib_call_task_cb(), implemented in bridge.c.
 */
typedef void (*milib_task_cb)(int64_t task_id, const char* result_json, void* user);

void milib_call_task_cb(milib_task_cb cb, int64_t task_id, const char* result_json, void* user);

/* Static storage for milib_version(), which must not be freed by the caller. */
const char* milib_version_cstr(void);

/*
 * Opaque SharedVPNLink handle. The struct lives in C memory so the caller can
 * keep it across calls; the library keeps the actual state.
 */
typedef struct milib_link {
    uint64_t id;
    uint64_t core;
} milib_link;

milib_link* milib_link_new(uint64_t id, uint64_t core);
void milib_link_free(milib_link* link);
uint64_t milib_link_get_id(const milib_link* link);
uint64_t milib_link_get_core(const milib_link* link);

#endif /* MILIB_BRIDGE_H */
