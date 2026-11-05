#ifndef MILIB_SUBS_H
#define MILIB_SUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "milib.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define MILIB_SUBS_API __declspec(dllexport)
#else
#define MILIB_SUBS_API __attribute__((visibility("default")))
#endif

typedef struct milib_subs milib_subs;

typedef struct milib_subs_config {
    const char* db_path;
    const char* user_agent;
    const char* test_url;
    const char* test_expected;
    int test_timeout_ms;
    int request_limit;              /* 0 disables the limiter */
    int request_window_seconds;
    int default_fetch_interval_sec;
} milib_subs_config;

MILIB_SUBS_API milib_subs_config milib_subs_config_default(void);

typedef struct milib_subs_open_result {
    milib_subs* db;
    milib_error error;
} milib_subs_open_result;

MILIB_SUBS_API milib_subs_open_result milib_subs_open(const milib_subs_config* config);
MILIB_SUBS_API void milib_subs_close(milib_subs* db);

/* Subscriptions. */
MILIB_SUBS_API milib_error milib_subs_add_subscription(milib_subs* db,
                                                       const char* url,
                                                       int priority,
                                                       const char* name);
MILIB_SUBS_API milib_error milib_subs_remove_subscription(milib_subs* db, int64_t id);
MILIB_SUBS_API milib_error milib_subs_set_priority(milib_subs* db, int64_t id, int priority);
MILIB_SUBS_API milib_error milib_subs_set_enabled(milib_subs* db, int64_t id, bool enabled);

typedef struct milib_subscription {
    int64_t id;
    char url[MILIB_URL_MAX];
    char name[MILIB_NAME_MAX];
    int priority;
    bool enabled;
    int64_t last_fetch_at;
    int64_t next_fetch_at;
    int last_status;
    int failure_count;
    char etag[256];
    char last_modified[256];
    char userinfo[512];
    char last_error[MILIB_ERROR_MAX];
} milib_subscription;

typedef struct milib_subscription_list {
    milib_error error;
    milib_subscription* items;
    size_t count;
} milib_subscription_list;

MILIB_SUBS_API milib_subscription_list milib_subs_load_subscriptions(milib_subs* db);
MILIB_SUBS_API void milib_subs_unload_subscriptions(milib_subscription_list list);

/* Proxies. */
typedef struct milib_subs_proxy {
    int64_t id;
    int64_t subscription_id;
    char link[MILIB_LINK_MAX];
    char name[MILIB_NAME_MAX];
    char protocol[MILIB_TYPE_MAX];
    char server[MILIB_HOST_MAX];
    int port;
    int rating;
    bool alive;
    int64_t last_tested_at;
    unsigned last_delay_ms;
    char last_error[MILIB_ERROR_MAX];
} milib_subs_proxy;

typedef struct milib_subs_proxy_list {
    milib_error error;
    milib_subs_proxy* items;
    size_t count;
} milib_subs_proxy_list;

MILIB_SUBS_API milib_subs_proxy_list milib_subs_load_proxies(milib_subs* db,
                                                             int64_t subscription_id,
                                                             size_t limit,
                                                             size_t offset);
MILIB_SUBS_API void milib_subs_unload_proxies(milib_subs_proxy_list list);

/* Fetch. */
typedef struct milib_subs_fetch_result {
    milib_error error;
    int64_t subscription_id;
    int http_status;
    size_t added;
    size_t updated;
    size_t removed;
    size_t skipped;
} milib_subs_fetch_result;

MILIB_SUBS_API milib_subs_fetch_result milib_subs_fetch(milib_subs* db, int64_t subscription_id);
MILIB_SUBS_API milib_subs_fetch_result milib_subs_fetch_due(milib_subs* db);

/* Testing and rating. */
typedef struct milib_subs_test_result {
    milib_error error;
    int64_t proxy_id;
    bool alive;
    unsigned delay_ms;
    int rating_before;
    int rating_after;
    int rating_delta;
    char test_error[MILIB_ERROR_MAX];
    char name[MILIB_NAME_MAX];
    char protocol[MILIB_TYPE_MAX];
    char server[MILIB_HOST_MAX];
    int port;
} milib_subs_test_result;

MILIB_SUBS_API milib_subs_test_result milib_subs_test_next(milib_subs* db,
                                                           milib_instance* milib);
MILIB_SUBS_API milib_subs_test_result milib_subs_test_proxy(milib_subs* db,
                                                            milib_instance* milib,
                                                            int64_t proxy_id);
MILIB_SUBS_API milib_subs_test_result milib_subs_record_test(milib_subs* db,
                                                             int64_t proxy_id,
                                                             bool alive,
                                                             unsigned delay_ms,
                                                             const char* error_text);

/* Shared rate limiter. Called automatically by fetch and test_next. */
MILIB_SUBS_API milib_error milib_subs_rate_limit_wait(milib_subs* db);

/* Stats. */
typedef struct milib_subs_stats {
    int64_t subscriptions;
    int64_t proxies;
    int64_t alive;
    int64_t dead;
    int64_t tests;
} milib_subs_stats;

MILIB_SUBS_API milib_subs_stats milib_subs_get_stats(milib_subs* db);

#ifdef __cplusplus
}
#endif
#endif /* MILIB_SUBS_H */
