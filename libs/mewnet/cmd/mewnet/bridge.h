#ifndef MEWNET_BRIDGE_H
#define MEWNET_BRIDGE_H

#include <stdint.h>
#include <stdlib.h>

#include "../../include/mewnet_types.h"

/*
 * Callback trampolines. Go code must not call a C function pointer directly,
 * so every callback is dispatched through these helpers.
 */
void mewnet_call_task_cb(mewnet_task_cb cb, const mewnet_task_result* result, void* user);
void mewnet_call_log_cb(mewnet_log_cb cb, mewnet_log_level level, const char* message, void* user);
int mewnet_call_http_cb(mewnet_http_cb cb, const void* data, size_t len, void* user);

/* Static storage for mewnet_version(), which must not be freed by the caller. */
const char* mewnet_version_cstr(void);

/* Opaque handles. The structs live in C memory and are owned by the caller. */
struct mewnet_instance {
    uint64_t handle;
};

struct mewnet_link {
    uint64_t id;
    uint64_t core;
};

struct mewnet_task {
    uint64_t id;
    uint64_t core;
    void* pings;
    size_t ping_count;
};

mewnet_instance* mewnet_instance_new(uint64_t handle);
void mewnet_instance_free(mewnet_instance* instance);
uint64_t mewnet_instance_get_handle(const mewnet_instance* instance);

mewnet_link* mewnet_link_new(uint64_t id, uint64_t core);
void mewnet_link_free(mewnet_link* link);
uint64_t mewnet_link_get_id(const mewnet_link* link);
uint64_t mewnet_link_get_core(const mewnet_link* link);

mewnet_task* mewnet_task_new(uint64_t id, uint64_t core);
void mewnet_task_free(mewnet_task* task);
uint64_t mewnet_task_get_id(const mewnet_task* task);
uint64_t mewnet_task_get_core(const mewnet_task* task);

#endif /* MEWNET_BRIDGE_H */
