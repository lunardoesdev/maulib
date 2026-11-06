#ifndef MEWNET_SUBS_H
#define MEWNET_SUBS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mewnet.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define MEWNET_SUBS_API __declspec(dllexport)
#else
#define MEWNET_SUBS_API __attribute__((visibility("default")))
#endif

typedef struct mewnet_subs mewnet_subs;

typedef struct mewnet_subs_config {
    const char* db_path;
    const char* user_agent;
    const char* test_url;
    const char* test_expected;
    int test_timeout_ms;
    int request_limit;              /* 0 disables the limiter */
    int request_window_seconds;
    int default_fetch_interval_sec;
} mewnet_subs_config;

MEWNET_SUBS_API mewnet_subs_config mewnet_subs_config_default(void);

typedef struct mewnet_subs_open_result {
    mewnet_subs* db;
    mewnet_error error;
} mewnet_subs_open_result;

MEWNET_SUBS_API mewnet_subs_open_result mewnet_subs_open(const mewnet_subs_config* config);
MEWNET_SUBS_API void mewnet_subs_close(mewnet_subs* db);

/* Subscription groups. */
typedef struct mewnet_subs_group {
    int64_t id;
    char name[MEWNET_NAME_MAX];
    int priority;
    bool enabled;
    int64_t created_at;
} mewnet_subs_group;

typedef struct mewnet_subs_group_list {
    mewnet_error error;
    mewnet_subs_group* items;
    size_t count;
} mewnet_subs_group_list;

MEWNET_SUBS_API mewnet_subs_group_list mewnet_subs_load_groups(mewnet_subs* db);
MEWNET_SUBS_API void mewnet_subs_unload_groups(mewnet_subs_group_list list);
MEWNET_SUBS_API mewnet_error mewnet_subs_add_group(mewnet_subs* db, const char* name, int priority);
MEWNET_SUBS_API mewnet_error mewnet_subs_remove_group(mewnet_subs* db, int64_t group_id);
MEWNET_SUBS_API mewnet_error mewnet_subs_set_group_priority(mewnet_subs* db, int64_t group_id, int priority);
MEWNET_SUBS_API mewnet_error mewnet_subs_set_subscription_group(mewnet_subs* db, int64_t subscription_id, int64_t group_id);
MEWNET_SUBS_API mewnet_error mewnet_subs_default_group(mewnet_subs* db, int64_t* out_group_id);

/* Subscriptions. */
MEWNET_SUBS_API mewnet_error mewnet_subs_add_subscription(mewnet_subs* db,
                                                       const char* url,
                                                       int priority,
                                                       const char* name);
MEWNET_SUBS_API mewnet_error mewnet_subs_add_subscription_in_group(mewnet_subs* db,
                                                                int64_t group_id,
                                                                const char* url,
                                                                int priority,
                                                                const char* name);
MEWNET_SUBS_API mewnet_error mewnet_subs_remove_subscription(mewnet_subs* db, int64_t id);
MEWNET_SUBS_API mewnet_error mewnet_subs_set_priority(mewnet_subs* db, int64_t id, int priority);
MEWNET_SUBS_API mewnet_error mewnet_subs_set_enabled(mewnet_subs* db, int64_t id, bool enabled);

typedef struct mewnet_subscription {
    int64_t id;
    int64_t group_id;
    char url[MEWNET_URL_MAX];
    char name[MEWNET_NAME_MAX];
    int priority;
    bool enabled;
    int64_t last_fetch_at;
    int64_t next_fetch_at;
    int last_status;
    int failure_count;
    char etag[256];
    char last_modified[256];
    char userinfo[512];
    char last_error[MEWNET_ERROR_MAX];
} mewnet_subscription;

typedef struct mewnet_subscription_list {
    mewnet_error error;
    mewnet_subscription* items;
    size_t count;
} mewnet_subscription_list;

MEWNET_SUBS_API mewnet_subscription_list mewnet_subs_load_subscriptions(mewnet_subs* db);
MEWNET_SUBS_API mewnet_subscription_list mewnet_subs_load_subscriptions_in_group(mewnet_subs* db, int64_t group_id);
MEWNET_SUBS_API void mewnet_subs_unload_subscriptions(mewnet_subscription_list list);

/* Proxies. */
typedef struct mewnet_subs_proxy {
    int64_t id;
    int64_t group_id;
    char link[MEWNET_LINK_MAX];
    char name[MEWNET_NAME_MAX];
    char protocol[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port;
    int rating;
    bool alive;
    bool manual;
    int64_t last_tested_at;
    unsigned last_delay_ms;
    char last_error[MEWNET_ERROR_MAX];
} mewnet_subs_proxy;

typedef struct mewnet_subs_proxy_list {
    mewnet_error error;
    mewnet_subs_proxy* items;
    size_t count;
} mewnet_subs_proxy_list;

MEWNET_SUBS_API mewnet_subs_proxy_list mewnet_subs_load_proxies(mewnet_subs* db,
                                                             int64_t group_id,
                                                             size_t limit,
                                                             size_t offset);
MEWNET_SUBS_API void mewnet_subs_unload_proxies(mewnet_subs_proxy_list list);

typedef struct mewnet_subs_proxy_summary {
    int64_t id;
    int64_t group_id;
    char name[MEWNET_NAME_MAX];
    char protocol[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port;
    int rating;
    bool alive;
    bool manual;
    int64_t last_tested_at;
    unsigned last_delay_ms;
    char last_error[MEWNET_ERROR_MAX];
} mewnet_subs_proxy_summary;

typedef struct mewnet_subs_proxy_summary_list {
    mewnet_error error;
    mewnet_subs_proxy_summary* items;
    size_t count;
} mewnet_subs_proxy_summary_list;

MEWNET_SUBS_API mewnet_subs_proxy_summary_list mewnet_subs_load_proxy_summaries(
    mewnet_subs* db, int64_t group_id, size_t limit, size_t offset);
MEWNET_SUBS_API void mewnet_subs_unload_proxy_summaries(mewnet_subs_proxy_summary_list list);

MEWNET_SUBS_API mewnet_error mewnet_subs_get_proxy(mewnet_subs* db, int64_t proxy_id,
                                                mewnet_subs_proxy* out);
MEWNET_SUBS_API mewnet_error mewnet_subs_add_proxy(mewnet_subs* db, int64_t group_id,
                                                const char* link);
MEWNET_SUBS_API mewnet_error mewnet_subs_remove_proxy(mewnet_subs* db, int64_t proxy_id);
MEWNET_SUBS_API mewnet_error mewnet_subs_count_proxies(mewnet_subs* db, int64_t group_id,
                                                    int64_t* out_count);

/* Fetch. */
typedef struct mewnet_subs_fetch_result {
    mewnet_error error;
    int64_t subscription_id;
    int http_status;
    size_t added;
    size_t updated;
    size_t removed;
    size_t skipped;
} mewnet_subs_fetch_result;

MEWNET_SUBS_API mewnet_subs_fetch_result mewnet_subs_fetch(mewnet_subs* db, int64_t subscription_id);
/* Fetch without waiting for the shared request limiter (discovery fallback). */
MEWNET_SUBS_API mewnet_subs_fetch_result mewnet_subs_fetch_now(mewnet_subs* db, int64_t subscription_id);
MEWNET_SUBS_API mewnet_subs_fetch_result mewnet_subs_fetch_due(mewnet_subs* db);

/* Discovery batch testing. */
typedef struct mewnet_subs_batch_test_result {
    mewnet_error error;
    size_t tested;
    size_t alive;
    int64_t first_alive_proxy_id;
    char first_alive_link[MEWNET_LINK_MAX];
    char first_alive_name[MEWNET_NAME_MAX];
} mewnet_subs_batch_test_result;

MEWNET_SUBS_API mewnet_subs_batch_test_result mewnet_subs_test_batch(
    mewnet_subs* db, mewnet_instance* milib, int64_t group_id,
    size_t limit, size_t concurrency);
MEWNET_SUBS_API mewnet_error mewnet_subs_count_untested(mewnet_subs* db, int64_t group_id,
                                                        int64_t* out_count);
MEWNET_SUBS_API mewnet_error mewnet_subs_count_alive(mewnet_subs* db, int64_t group_id,
                                                     int64_t* out_count);
MEWNET_SUBS_API mewnet_error mewnet_subs_first_alive(mewnet_subs* db, int64_t group_id,
                                                     mewnet_subs_proxy* out);

/* Testing and rating. */
typedef struct mewnet_subs_test_result {
    mewnet_error error;
    int64_t proxy_id;
    bool alive;
    unsigned delay_ms;
    int rating_before;
    int rating_after;
    int rating_delta;
    char test_error[MEWNET_ERROR_MAX];
    char name[MEWNET_NAME_MAX];
    char protocol[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port;
} mewnet_subs_test_result;

MEWNET_SUBS_API mewnet_subs_test_result mewnet_subs_test_next(mewnet_subs* db,
                                                           mewnet_instance* mewnet);
MEWNET_SUBS_API mewnet_subs_test_result mewnet_subs_test_proxy(mewnet_subs* db,
                                                            mewnet_instance* mewnet,
                                                            int64_t proxy_id);
MEWNET_SUBS_API mewnet_subs_test_result mewnet_subs_record_test(mewnet_subs* db,
                                                             int64_t proxy_id,
                                                             bool alive,
                                                             unsigned delay_ms,
                                                             const char* error_text);

/* Shared rate limiter. Called automatically by fetch and test_next. */
MEWNET_SUBS_API mewnet_error mewnet_subs_rate_limit_wait(mewnet_subs* db);

/* Stats. */
typedef struct mewnet_subs_stats {
    int64_t subscriptions;
    int64_t proxies;
    int64_t alive;
    int64_t dead;
    int64_t tests;
} mewnet_subs_stats;

MEWNET_SUBS_API mewnet_subs_stats mewnet_subs_get_stats(mewnet_subs* db);

#ifdef __cplusplus
}
#endif
#endif /* MEWNET_SUBS_H */
