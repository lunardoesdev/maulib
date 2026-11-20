#include "bridge.h"

void mewnet_call_task_cb(mewnet_task_cb cb, const mewnet_task_result* result, void* user) {
    if (cb == NULL) {
        return;
    }
    cb(result, user);
}

void mewnet_call_log_cb(mewnet_log_cb cb, mewnet_log_level level, const char* message, void* user) {
    if (cb == NULL) {
        return;
    }
    cb(level, message, user);
}

int mewnet_call_http_cb(mewnet_http_cb cb, const void* data, size_t len, void* user) {
    if (cb == NULL) {
        return 0;
    }
    return cb(data, len, user);
}

const char* mewnet_version_cstr(void) {
    return "0.2.0";
}

mewnet_instance* mewnet_instance_new(uint64_t handle) {
    mewnet_instance* instance = (mewnet_instance*)malloc(sizeof(mewnet_instance));
    if (instance == NULL) {
        return NULL;
    }
    instance->handle = handle;
    return instance;
}

void mewnet_instance_free(mewnet_instance* instance) {
    free(instance);
}

uint64_t mewnet_instance_get_handle(const mewnet_instance* instance) {
    return instance == NULL ? 0 : instance->handle;
}

mewnet_link* mewnet_link_new(uint64_t id, uint64_t core) {
    mewnet_link* link = (mewnet_link*)malloc(sizeof(mewnet_link));
    if (link == NULL) {
        return NULL;
    }
    link->id = id;
    link->core = core;
    return link;
}

void mewnet_link_free(mewnet_link* link) {
    free(link);
}

uint64_t mewnet_link_get_id(const mewnet_link* link) {
    return link == NULL ? 0 : link->id;
}

uint64_t mewnet_link_get_core(const mewnet_link* link) {
    return link == NULL ? 0 : link->core;
}

mewnet_task* mewnet_task_new(uint64_t id, uint64_t core) {
    mewnet_task* task = (mewnet_task*)malloc(sizeof(mewnet_task));
    if (task == NULL) {
        return NULL;
    }
    task->id = id;
    task->core = core;
    task->pings = NULL;
    task->ping_count = 0;
    return task;
}

void mewnet_task_free(mewnet_task* task) {
    free(task);
}

uint64_t mewnet_task_get_id(const mewnet_task* task) {
    return task == NULL ? 0 : task->id;
}

uint64_t mewnet_task_get_core(const mewnet_task* task) {
    return task == NULL ? 0 : task->core;
}
