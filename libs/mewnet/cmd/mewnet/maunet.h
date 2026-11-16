#ifndef MAUNET_H
#define MAUNET_H

/*
 * maunet - a raylib-style global façade over mewnet.
 *
 * mewnet gives you handles: an instance, subscription database, link objects.
 * maunet keeps that state in the library, so application code only deals with
 * link ids:
 *
 *     MaunetSettings settings = MaunetSettingsDefault();
 *     settings.db_path = "data/app.db";
 *     if (!InitMaunet(&settings)) { ... GetMaunetError() ... }
 *
 *     int64_t link = AddMaunetLink("vless://...").id;
 *     SetMaunetLinkAsDefault(link);
 *     // traffic now goes through http://127.0.0.1:<GetMaunetProxyPort()>
 *
 *     DeinitMaunet();
 *
 * Conventions, same as mewnet: functions return false or a result struct with
 * an error, lists are released with the matching Unload function, and ids stay
 * valid while the link exists in the database.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mewnet.h"
#include "mewnet_subs.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define MAUNET_API __declspec(dllexport)
#else
#define MAUNET_API __attribute__((visibility("default")))
#endif

/* Settings for the hidden library state. Zero fields mean "use the default". */
typedef struct maunet_settings {
    const char* db_path;     /* NULL: "data/maunet.db"; created and migrated */
    int mixed_port;          /* 0: free port, -1: no inbound, >0: exact port */
    const char* log_level;   /* NULL: "warning" */
    const char* user_agent;  /* NULL: "maunet/0.1" */

    const char* test_url;      /* NULL: gstatic generate_204 */
    const char* test_expected; /* NULL: "204" */
    int test_timeout_ms;       /* 0: 5000 */

    const char* health_url;      /* NULL: test_url */
    const char* health_expected; /* NULL: test_expected */
    int health_timeout_ms;       /* 0: 2500 */

    int request_limit;          /* 0 disables the shared limiter */
    int request_window_seconds; /* 0: 240 */
    int fetch_interval_sec;     /* 0: 21600 (6 h) */

    int pool_target;    /* agent: working nodes to keep, 0: 12 */
    size_t batch;       /* agent: configs per batch, 0: 70 */
    size_t concurrency; /* agent: parallel checks, 0: 70 */
    bool run_agent;     /* start the discovery agent together with the core */
} maunet_settings;

/* One stored link. `id` is the id you pass to the link functions. */
typedef struct maunet_link_info {
    int64_t id;
    char name[MEWNET_NAME_MAX];
    char protocol[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port;
    int rating;
    bool alive;
    bool manual;   /* added directly with AddMaunetLink, not from a subscription */
    bool is_default;
    bool in_core;  /* currently registered in the running core */
    unsigned delay_ms;
    char last_error[MEWNET_ERROR_MAX];
} maunet_link_info;

typedef struct maunet_link_list {
    bool ok;
    mewnet_error_code error;
    maunet_link_info* items;
    size_t count;
} maunet_link_list;

typedef struct maunet_link_result {
    int64_t id;      /* 0 on failure */
    mewnet_error error;
} maunet_link_result;

typedef struct maunet_subscription_info {
    int64_t id;
    char url[MEWNET_URL_MAX];
    char name[MEWNET_NAME_MAX];
    int priority;
    bool enabled;
    int status; /* last HTTP status, 0 if never fetched */
    int failure_count;
    int64_t last_fetch_at;
    char last_error[MEWNET_ERROR_MAX];
} maunet_subscription_info;

typedef struct maunet_subscription_list {
    bool ok;
    mewnet_error_code error;
    maunet_subscription_info* items;
    size_t count;
} maunet_subscription_list;

typedef struct maunet_status {
    bool ready;
    bool agent_running;
    int proxy_port;          /* mixed inbound, -1 when disabled */
    int64_t links;           /* stored configs, manual and from subscriptions */
    int64_t subscriptions;
    int64_t alive;
    int64_t default_link;    /* 0 when nothing is selected */
    char default_name[MEWNET_NAME_MAX];
    char selected[MEWNET_NAME_MAX];
} maunet_status;

/* ---------------------------------------------------------------- */
/* Lifecycle                                                        */
/* ---------------------------------------------------------------- */

MAUNET_API maunet_settings MaunetSettingsDefault(void);

/* Open the database, start the core (and the agent when run_agent is set). */
MAUNET_API bool InitMaunet(const maunet_settings* settings);

/* Stop the agent, close the core and the database. Safe to call twice. */
MAUNET_API void DeinitMaunet(void);

MAUNET_API bool IsMaunetReady(void);

/* Message of the last failed call, "" after a successful one. */
MAUNET_API const char* GetMaunetError(void);

MAUNET_API maunet_status GetMaunetStatus(void);

/* Local mixed port, or -1 when the inbound is disabled. */
MAUNET_API int GetMaunetProxyPort(void);

/* ---------------------------------------------------------------- */
/* Links                                                            */
/* ---------------------------------------------------------------- */

/*
 * Store a share link and return its id. The link is written to the database as
 * a manual config, so it survives restarts and is not removed by a
 * subscription refresh. It is registered in the core lazily, when it becomes
 * the default or is tested.
 */
MAUNET_API maunet_link_result AddMaunetLink(const char* uri);

/* Remove the stored link and drop it from the core if it was there. */
MAUNET_API bool RemoveMaunetLink(int64_t link_id);

/*
 * Register the link in the core and select it: the mixed inbound starts using
 * it immediately. Existing connections keep their old node until they close.
 */
MAUNET_API bool SetMaunetLinkAsDefault(int64_t link_id);

/* Current default link id, or 0. Follows the agent when it switches nodes. */
MAUNET_API int64_t GetMaunetDefaultLink(void);

/*
 * Measure the link through the tunnel and update its rating. Returns true when
 * the link answered within the configured timeout.
 */
MAUNET_API bool TestMaunetLink(int64_t link_id);

MAUNET_API maunet_link_info GetMaunetLink(int64_t link_id);

/* Page through the stored links; release with UnloadMaunetLinks. */
MAUNET_API maunet_link_list LoadMaunetLinks(size_t limit, size_t offset);
MAUNET_API void UnloadMaunetLinks(maunet_link_list list);

MAUNET_API int64_t GetMaunetLinkCount(void);

/* ---------------------------------------------------------------- */
/* Subscriptions                                                    */
/* ---------------------------------------------------------------- */

/*
 * Store a subscription URL, fetch it once and return its id. Configs that
 * arrive keep their own link ids and show up in LoadMaunetLinks. A fetch
 * failure is reported in the result but does not discard the subscription.
 */
MAUNET_API maunet_link_result AddMaunetSubscription(const char *url);

MAUNET_API bool RemoveMaunetSubscription(int64_t subscription_id);

/* Fetch every subscription now, bypassing the shared request budget. */
MAUNET_API bool RefreshMaunetSubscriptions(void);

MAUNET_API maunet_subscription_list LoadMaunetSubscriptions(void);
MAUNET_API void UnloadMaunetSubscriptions(maunet_subscription_list list);

MAUNET_API int64_t GetMaunetSubscriptionCount(void);

#ifdef __cplusplus
}
#endif

#endif /* MAUNET_H */
