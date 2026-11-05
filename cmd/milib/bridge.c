#include "bridge.h"

void milib_call_task_cb(milib_task_cb cb, const milib_task_result* result, void* user) {
    if (cb == NULL) {
        return;
    }
    cb(result, user);
}

void milib_call_log_cb(milib_log_cb cb, milib_log_level level, const char* message, void* user) {
    if (cb == NULL) {
        return;
    }
    cb(level, message, user);
}

int milib_call_http_cb(milib_http_cb cb, const void* data, size_t len, void* user) {
    if (cb == NULL) {
        return 0;
    }
    return cb(data, len, user);
}

const char* milib_version_cstr(void) {
    return "0.2.0";
}

milib_instance* milib_instance_new(uint64_t handle) {
    milib_instance* instance = (milib_instance*)malloc(sizeof(milib_instance));
    if (instance == NULL) {
        return NULL;
    }
    instance->handle = handle;
    return instance;
}

void milib_instance_free(milib_instance* instance) {
    free(instance);
}

uint64_t milib_instance_get_handle(const milib_instance* instance) {
    return instance == NULL ? 0 : instance->handle;
}

milib_link* milib_link_new(uint64_t id, uint64_t core) {
    milib_link* link = (milib_link*)malloc(sizeof(milib_link));
    if (link == NULL) {
        return NULL;
    }
    link->id = id;
    link->core = core;
    return link;
}

void milib_link_free(milib_link* link) {
    free(link);
}

uint64_t milib_link_get_id(const milib_link* link) {
    return link == NULL ? 0 : link->id;
}

uint64_t milib_link_get_core(const milib_link* link) {
    return link == NULL ? 0 : link->core;
}

milib_task* milib_task_new(uint64_t id, uint64_t core) {
    milib_task* task = (milib_task*)malloc(sizeof(milib_task));
    if (task == NULL) {
        return NULL;
    }
    task->id = id;
    task->core = core;
    task->pings = NULL;
    task->ping_count = 0;
    return task;
}

void milib_task_free(milib_task* task) {
    free(task);
}

uint64_t milib_task_get_id(const milib_task* task) {
    return task == NULL ? 0 : task->id;
}

uint64_t milib_task_get_core(const milib_task* task) {
    return task == NULL ? 0 : task->core;
}
