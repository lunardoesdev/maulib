#include "bridge.h"

void milib_call_task_cb(milib_task_cb cb, int64_t task_id, const char* result_json, void* user) {
    if (cb == NULL) {
        return;
    }
    cb(task_id, result_json, user);
}

const char* milib_version_cstr(void) {
    return "0.1.0";
}

milib_link* milib_link_new(uint64_t id, uint64_t core) {
    milib_link *link = (milib_link*)malloc(sizeof(milib_link));
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
