#ifndef MILIB_BRIDGE_H
#define MILIB_BRIDGE_H

#include <stdint.h>
#include <stdlib.h>

#include "../../include/milib_types.h"

/*
 * Callback trampolines. Go code must not call a C function pointer directly,
 * so every callback is dispatched through these helpers.
 */
void milib_call_task_cb(milib_task_cb cb, const milib_task_result* result, void* user);
void milib_call_log_cb(milib_log_cb cb, milib_log_level level, const char* message, void* user);
int milib_call_http_cb(milib_http_cb cb, const void* data, size_t len, void* user);

/* Static storage for milib_version(), which must not be freed by the caller. */
const char* milib_version_cstr(void);

/* Opaque handles. The structs live in C memory and are owned by the caller. */
struct milib_instance {
    uint64_t handle;
};

struct milib_link {
    uint64_t id;
    uint64_t core;
};

struct milib_task {
    uint64_t id;
    uint64_t core;
    void* pings;
    size_t ping_count;
};

milib_instance* milib_instance_new(uint64_t handle);
void milib_instance_free(milib_instance* instance);
uint64_t milib_instance_get_handle(const milib_instance* instance);

milib_link* milib_link_new(uint64_t id, uint64_t core);
void milib_link_free(milib_link* link);
uint64_t milib_link_get_id(const milib_link* link);
uint64_t milib_link_get_core(const milib_link* link);

milib_task* milib_task_new(uint64_t id, uint64_t core);
void milib_task_free(milib_task* task);
uint64_t milib_task_get_id(const milib_task* task);
uint64_t milib_task_get_core(const milib_task* task);

#endif /* MILIB_BRIDGE_H */
