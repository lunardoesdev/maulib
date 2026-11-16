/*
 * maunet.c - global state and link-centric API on top of mewnet.
 *
 * All state lives in this file: one core instance, one subscription database,
 * an optional discovery agent and the id of the link that is currently
 * selected. Application code only passes ids around.
 */

#include "maunet.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MAUNET_DEFAULT_DB "data/maunet.db"
#define MAUNET_DEFAULT_UA "maunet/0.1"

typedef struct maunet_state {
    bool ready;
    bool agent_running;

    char db_path[512];
    char log_level[32];
    char user_agent[128];
    char test_url[MEWNET_URL_MAX];
    char test_expected[MEWNET_TYPE_MAX];
    char health_url[MEWNET_URL_MAX];
    char health_expected[MEWNET_TYPE_MAX];
    char error[MEWNET_ERROR_MAX];

    int mixed_port;
    int test_timeout_ms;
    int health_timeout_ms;
    int request_limit;
    int request_window_seconds;
    int fetch_interval_sec;
    int pool_target;
    size_t batch;
    size_t concurrency;

    int64_t group;
    int64_t default_link;

    mewnet_subs* db;
    mewnet_instance* core;
    mewnet_agent* agent;
} maunet_state;

static maunet_state g;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------------------------------------------------------------- */
/* Small helpers                                                    */
/* ---------------------------------------------------------------- */

static void set_error(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g.error, sizeof(g.error), fmt, args);
    va_end(args);
}

static void clear_error(void) {
    g.error[0] = '\0';
}

static void copy_text(char* dst, size_t size, const char* src, const char* fallback) {
    const char* value = (src != NULL && src[0] != '\0') ? src : fallback;
    if (size == 0) {
        return;
    }
    snprintf(dst, size, "%s", value != NULL ? value : "");
}

static void make_parent_dirs(const char* path) {
    if (path == NULL) {
        return;
    }
    char buffer[512];
    snprintf(buffer, sizeof(buffer), "%s", path);
    char* slash = strrchr(buffer, '/');
    if (slash == NULL) {
        return;
    }
    *slash = '\0';
    if (buffer[0] == '\0') {
        return;
    }
    mkdir(buffer, 0755);
}

static bool ready_unlocked(void) {
    if (!g.ready) {
        set_error("maunet is not initialized; call InitMaunet first");
        return false;
    }
    return true;
}

/* Add the stored link to the core if it is missing. Caller holds the lock. */
static bool ensure_in_core(int64_t link_id, const mewnet_subs_proxy* row) {
    if (mewnet_has_proxy(g.core, row->name)) {
        return true;
    }
    mewnet_add_options opts = mewnet_add_options_default();
    opts.name = row->name;
    opts.select_proxy = false;
    mewnet_error_code rc = mewnet_add_proxy(g.core, row->link, &opts);
    if (rc != MEWNET_ERROR_NONE) {
        set_error("add link %lld to the core failed (code %d)", (long long)link_id, (int)rc);
        return false;
    }
    if (!mewnet_has_proxy(g.core, row->name)) {
        set_error("link %lld is registered under a different name", (long long)link_id);
        return false;
    }
    return true;
}

static int64_t find_subscription_by_url(const char* url) {
    mewnet_subscription_list subs = mewnet_subs_load_subscriptions(g.db);
    int64_t id = 0;
    for (size_t i = 0; i < subs.count; i++) {
        if (strcmp(subs.items[i].url, url) == 0) {
            id = subs.items[i].id;
            break;
        }
    }
    mewnet_subs_unload_subscriptions(subs);
    return id;
}

/* ---------------------------------------------------------------- */
/* Lifecycle                                                        */
/* ---------------------------------------------------------------- */

maunet_settings MaunetSettingsDefault(void) {
    maunet_settings s;
    memset(&s, 0, sizeof(s));
    s.db_path = MAUNET_DEFAULT_DB;
    s.mixed_port = 0;
    s.log_level = "warning";
    s.user_agent = MAUNET_DEFAULT_UA;
    s.test_url = "https://www.gstatic.com/generate_204";
    s.test_expected = "204";
    s.test_timeout_ms = 5000;
    s.health_url = "https://www.gstatic.com/generate_204";
    s.health_expected = "204";
    s.health_timeout_ms = 2500;
    s.request_limit = 10;
    s.request_window_seconds = 240;
    s.fetch_interval_sec = 21600;
    s.pool_target = 12;
    s.batch = 70;
    s.concurrency = 70;
    s.run_agent = false;
    return s;
}

bool IsMaunetReady(void) {
    bool ready;
    pthread_mutex_lock(&g_lock);
    ready = g.ready;
    pthread_mutex_unlock(&g_lock);
    return ready;
}

const char* GetMaunetError(void) {
    return g.error;
}

bool InitMaunet(const maunet_settings* settings) {
    maunet_settings s = MaunetSettingsDefault();
    if (settings != NULL) {
        if (settings->db_path != NULL && settings->db_path[0] != '\0') {
            s.db_path = settings->db_path;
        }
        if (settings->log_level != NULL && settings->log_level[0] != '\0') {
            s.log_level = settings->log_level;
        }
        if (settings->user_agent != NULL && settings->user_agent[0] != '\0') {
            s.user_agent = settings->user_agent;
        }
        if (settings->test_url != NULL && settings->test_url[0] != '\0') {
            s.test_url = settings->test_url;
        }
        if (settings->test_expected != NULL && settings->test_expected[0] != '\0') {
            s.test_expected = settings->test_expected;
        }
        if (settings->health_url != NULL && settings->health_url[0] != '\0') {
            s.health_url = settings->health_url;
        }
        if (settings->health_expected != NULL && settings->health_expected[0] != '\0') {
            s.health_expected = settings->health_expected;
        }
        if (settings->mixed_port != 0) {
            s.mixed_port = settings->mixed_port;
        }
        if (settings->test_timeout_ms > 0) {
            s.test_timeout_ms = settings->test_timeout_ms;
        }
        if (settings->health_timeout_ms > 0) {
            s.health_timeout_ms = settings->health_timeout_ms;
        }
        if (settings->request_limit > 0) {
            s.request_limit = settings->request_limit;
        }
        if (settings->request_window_seconds > 0) {
            s.request_window_seconds = settings->request_window_seconds;
        }
        if (settings->fetch_interval_sec > 0) {
            s.fetch_interval_sec = settings->fetch_interval_sec;
        }
        if (settings->pool_target > 0) {
            s.pool_target = settings->pool_target;
        }
        if (settings->batch > 0) {
            s.batch = settings->batch;
        }
        if (settings->concurrency > 0) {
            s.concurrency = settings->concurrency;
        }
        s.run_agent = settings->run_agent;
    }

    if (s.run_agent && s.mixed_port < 0) {
        set_error("run_agent needs a mixed inbound; mixed_port is disabled");
        return false;
    }

    pthread_mutex_lock(&g_lock);
    if (g.ready) {
        set_error("maunet is already initialized");
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    memset(&g, 0, sizeof(g));
    copy_text(g.db_path, sizeof(g.db_path), s.db_path, MAUNET_DEFAULT_DB);
    copy_text(g.log_level, sizeof(g.log_level), s.log_level, "warning");
    copy_text(g.user_agent, sizeof(g.user_agent), s.user_agent, MAUNET_DEFAULT_UA);
    copy_text(g.test_url, sizeof(g.test_url), s.test_url,
              "https://www.gstatic.com/generate_204");
    copy_text(g.test_expected, sizeof(g.test_expected), s.test_expected, "204");
    copy_text(g.health_url, sizeof(g.health_url), s.health_url, g.test_url);
    copy_text(g.health_expected, sizeof(g.health_expected), s.health_expected,
              g.test_expected);
    g.mixed_port = s.mixed_port;
    g.test_timeout_ms = s.test_timeout_ms;
    g.health_timeout_ms = s.health_timeout_ms;
    g.request_limit = s.request_limit;
    g.request_window_seconds = s.request_window_seconds;
    g.fetch_interval_sec = s.fetch_interval_sec;
    g.pool_target = s.pool_target;
    g.batch = s.batch;
    g.concurrency = s.concurrency;

    make_parent_dirs(g.db_path);

    mewnet_subs_config db_config = mewnet_subs_config_default();
    db_config.db_path = g.db_path;
    db_config.user_agent = g.user_agent;
    db_config.test_url = g.test_url;
    db_config.test_expected = g.test_expected;
    db_config.test_timeout_ms = g.test_timeout_ms;
    db_config.request_limit = g.request_limit;
    db_config.request_window_seconds = g.request_window_seconds;
    db_config.default_fetch_interval_sec = g.fetch_interval_sec;

    mewnet_subs_open_result opened = mewnet_subs_open(&db_config);
    if (opened.db == NULL) {
        set_error("open %s: %s", g.db_path, opened.error.message);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    g.db = opened.db;

    mewnet_error err = mewnet_subs_default_group(g.db, &g.group);
    if (err.code != MEWNET_ERROR_NONE) {
        set_error("default group: %s", err.message);
        mewnet_subs_close(g.db);
        g.db = NULL;
        pthread_mutex_unlock(&g_lock);
        return false;
    }

    mewnet_config core_config = mewnet_config_default();
    core_config.mixed_port = g.mixed_port;
    core_config.log_level = g.log_level;
    mewnet_init_result init = mewnet_init(&core_config);
    if (init.instance == NULL) {
        set_error("core: %s", init.error.message);
        mewnet_subs_close(g.db);
        g.db = NULL;
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    g.core = init.instance;

    if (s.run_agent) {
        mewnet_agent_config agent_config = mewnet_agent_config_default();
        agent_config.db = g.db;
        agent_config.milib = g.core;
        agent_config.group_id = g.group;
        agent_config.proxy_port = mewnet_get_status(g.core).mixed_port;
        agent_config.batch = g.batch;
        agent_config.concurrency = g.concurrency;
        agent_config.fetch_interval_sec = g.fetch_interval_sec;
        agent_config.pool_target = g.pool_target;
        agent_config.health_interval_sec = 6;
        agent_config.health_timeout_ms = g.health_timeout_ms;
        agent_config.health_url = g.health_url;
        agent_config.health_expected = g.health_expected;
        mewnet_agent_callbacks callbacks;
        memset(&callbacks, 0, sizeof(callbacks));
        g.agent = mewnet_agent_start(&agent_config, callbacks);
        if (g.agent == NULL) {
            set_error("discovery agent failed to start");
            mewnet_close(g.core);
            g.core = NULL;
            mewnet_subs_close(g.db);
            g.db = NULL;
            pthread_mutex_unlock(&g_lock);
            return false;
        }
        g.agent_running = true;
    }

    g.default_link = 0;
    g.ready = true;
    clear_error();
    pthread_mutex_unlock(&g_lock);
    return true;
}

void DeinitMaunet(void) {
    pthread_mutex_lock(&g_lock);
    if (!g.ready) {
        pthread_mutex_unlock(&g_lock);
        return;
    }
    if (g.agent != NULL) {
        mewnet_agent_stop(g.agent); /* frees the handle */
        g.agent = NULL;
        g.agent_running = false;
    }
    if (g.core != NULL) {
        mewnet_close(g.core);
        g.core = NULL;
    }
    if (g.db != NULL) {
        mewnet_subs_close(g.db);
        g.db = NULL;
    }
    g.ready = false;
    g.default_link = 0;
    g.group = 0;
    clear_error();
    pthread_mutex_unlock(&g_lock);
}

maunet_status GetMaunetStatus(void) {
    maunet_status status;
    memset(&status, 0, sizeof(status));
    pthread_mutex_lock(&g_lock);
    if (!g.ready) {
        pthread_mutex_unlock(&g_lock);
        return status;
    }
    mewnet_status_info core_status = mewnet_get_status(g.core);
    mewnet_subs_stats stats = mewnet_subs_get_stats(g.db);
    status.ready = true;
    status.agent_running = g.agent_running;
    status.proxy_port = core_status.mixed_port;
    status.links = stats.proxies;
    status.subscriptions = stats.subscriptions;
    status.alive = stats.alive;
    status.default_link = g.default_link;
    copy_text(status.selected, sizeof(status.selected), core_status.selected, "");
    if (g.default_link != 0) {
        mewnet_subs_proxy row;
        if (mewnet_subs_get_proxy(g.db, g.default_link, &row).code == MEWNET_ERROR_NONE) {
            copy_text(status.default_name, sizeof(status.default_name), row.name, "");
        }
    }
    pthread_mutex_unlock(&g_lock);
    return status;
}

int GetMaunetProxyPort(void) {
    int port = -1;
    pthread_mutex_lock(&g_lock);
    if (g.ready) {
        port = mewnet_get_status(g.core).mixed_port;
    }
    pthread_mutex_unlock(&g_lock);
    return port;
}

/* ---------------------------------------------------------------- */
/* Links                                                            */
/* ---------------------------------------------------------------- */

maunet_link_result AddMaunetLink(const char* uri) {
    maunet_link_result result;
    memset(&result, 0, sizeof(result));
    pthread_mutex_lock(&g_lock);
    if (!ready_unlocked()) {
        result.error.code = MEWNET_ERROR_CLOSED;
        copy_text(result.error.message, sizeof(result.error.message), g.error, "");
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    if (uri == NULL || uri[0] == '\0') {
        set_error("link is required");
        result.error.code = MEWNET_ERROR_INVALID_ARGUMENT;
        copy_text(result.error.message, sizeof(result.error.message), g.error, "");
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    int64_t id = 0;
    mewnet_error err = mewnet_subs_add_proxy_id(g.db, g.group, uri, &id);
    if (err.code != MEWNET_ERROR_NONE) {
        set_error("%s", err.message);
        result.error = err;
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    clear_error();
    result.id = id;
    result.error.code = MEWNET_ERROR_NONE;
    pthread_mutex_unlock(&g_lock);
    return result;
}

bool RemoveMaunetLink(int64_t link_id) {
    pthread_mutex_lock(&g_lock);
    if (!ready_unlocked()) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    mewnet_subs_proxy row;
    mewnet_error err = mewnet_subs_get_proxy(g.db, link_id, &row);
    if (err.code != MEWNET_ERROR_NONE) {
        set_error("%s", err.message);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (mewnet_has_proxy(g.core, row.name)) {
        mewnet_remove_proxy(g.core, row.name);
    }
    err = mewnet_subs_remove_proxy(g.db, link_id);
    if (err.code != MEWNET_ERROR_NONE) {
        set_error("%s", err.message);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (g.default_link == link_id) {
        g.default_link = 0;
        mewnet_select_proxy(g.core, "DIRECT");
    }
    clear_error();
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool SetMaunetLinkAsDefault(int64_t link_id) {
    pthread_mutex_lock(&g_lock);
    if (!ready_unlocked()) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    mewnet_subs_proxy row;
    mewnet_error err = mewnet_subs_get_proxy(g.db, link_id, &row);
    if (err.code != MEWNET_ERROR_NONE) {
        set_error("%s", err.message);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (!ensure_in_core(link_id, &row)) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (mewnet_select_proxy(g.core, row.name) != MEWNET_ERROR_NONE) {
        set_error("select %s failed", row.name);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    g.default_link = link_id;
    clear_error();
    pthread_mutex_unlock(&g_lock);
    return true;
}

int64_t GetMaunetDefaultLink(void) {
    int64_t result = 0;
    pthread_mutex_lock(&g_lock);
    if (!g.ready) {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    mewnet_status_info status = mewnet_get_status(g.core);
    char selected[MEWNET_NAME_MAX];
    copy_text(selected, sizeof(selected), status.selected, "");

    if (g.default_link != 0) {
        mewnet_subs_proxy row;
        if (mewnet_subs_get_proxy(g.db, g.default_link, &row).code == MEWNET_ERROR_NONE &&
            strcmp(row.name, selected) == 0) {
            result = g.default_link;
            pthread_mutex_unlock(&g_lock);
            return result;
        }
    }
    if (selected[0] != '\0') {
        mewnet_subs_proxy row;
        if (mewnet_subs_get_proxy_by_name(g.db, 0, selected, &row).code ==
            MEWNET_ERROR_NONE) {
            g.default_link = row.id; /* the agent moved the selection */
            result = row.id;
        }
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

bool TestMaunetLink(int64_t link_id) {
    pthread_mutex_lock(&g_lock);
    if (!ready_unlocked()) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    mewnet_subs_test_result test = mewnet_subs_test_proxy(g.db, g.core, link_id);
    if (test.error.code != MEWNET_ERROR_NONE) {
        set_error("%s", test.error.message);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    if (!test.alive) {
        set_error("%s", test.test_error[0] != '\0' ? test.test_error : "link is dead");
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    clear_error();
    pthread_mutex_unlock(&g_lock);
    return true;
}

maunet_link_info GetMaunetLink(int64_t link_id) {
    maunet_link_info info;
    memset(&info, 0, sizeof(info));
    pthread_mutex_lock(&g_lock);
    if (!g.ready) {
        pthread_mutex_unlock(&g_lock);
        return info;
    }
    mewnet_subs_proxy row;
    if (mewnet_subs_get_proxy(g.db, link_id, &row).code != MEWNET_ERROR_NONE) {
        pthread_mutex_unlock(&g_lock);
        return info;
    }
    info.id = row.id;
    copy_text(info.name, sizeof(info.name), row.name, "");
    copy_text(info.protocol, sizeof(info.protocol), row.protocol, "");
    copy_text(info.server, sizeof(info.server), row.server, "");
    info.port = row.port;
    info.rating = row.rating;
    info.alive = row.alive;
    info.manual = row.manual;
    info.delay_ms = row.last_delay_ms;
    copy_text(info.last_error, sizeof(info.last_error), row.last_error, "");
    info.in_core = mewnet_has_proxy(g.core, row.name);
    info.is_default = g.default_link == link_id;
    pthread_mutex_unlock(&g_lock);
    return info;
}

maunet_link_list LoadMaunetLinks(size_t limit, size_t offset) {
    maunet_link_list result;
    memset(&result, 0, sizeof(result));
    pthread_mutex_lock(&g_lock);
    if (!ready_unlocked()) {
        result.error = MEWNET_ERROR_FAILED;
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    if (limit == 0) {
        limit = 100;
    }
    mewnet_subs_proxy_list rows = mewnet_subs_load_proxies(g.db, 0, limit, offset);
    if (rows.error.code != MEWNET_ERROR_NONE) {
        set_error("%s", rows.error.message);
        result.error = rows.error.code;
        mewnet_subs_unload_proxies(rows);
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    maunet_link_info* items = NULL;
    if (rows.count > 0) {
        items = (maunet_link_info*)calloc(rows.count, sizeof(maunet_link_info));
    }
    if (items == NULL && rows.count > 0) {
        set_error("out of memory");
        result.error = MEWNET_ERROR_FAILED;
        mewnet_subs_unload_proxies(rows);
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    for (size_t i = 0; i < rows.count; i++) {
        const mewnet_subs_proxy* row = &rows.items[i];
        maunet_link_info* info = &items[i];
        info->id = row->id;
        copy_text(info->name, sizeof(info->name), row->name, "");
        copy_text(info->protocol, sizeof(info->protocol), row->protocol, "");
        copy_text(info->server, sizeof(info->server), row->server, "");
        info->port = row->port;
        info->rating = row->rating;
        info->alive = row->alive;
        info->manual = row->manual;
        info->delay_ms = row->last_delay_ms;
        copy_text(info->last_error, sizeof(info->last_error), row->last_error, "");
        info->in_core = mewnet_has_proxy(g.core, row->name);
        info->is_default = g.default_link == row->id;
    }
    mewnet_subs_unload_proxies(rows);
    result.ok = true;
    result.error = MEWNET_ERROR_NONE;
    result.items = items;
    result.count = rows.count;
    clear_error();
    pthread_mutex_unlock(&g_lock);
    return result;
}

void UnloadMaunetLinks(maunet_link_list list) {
    free(list.items);
}

int64_t GetMaunetLinkCount(void) {
    int64_t count = 0;
    pthread_mutex_lock(&g_lock);
    if (g.ready) {
        mewnet_subs_count_proxies(g.db, 0, &count);
    }
    pthread_mutex_unlock(&g_lock);
    return count;
}

/* ---------------------------------------------------------------- */
/* Subscriptions                                                    */
/* ---------------------------------------------------------------- */

maunet_link_result AddMaunetSubscription(const char* url) {
    maunet_link_result result;
    memset(&result, 0, sizeof(result));
    pthread_mutex_lock(&g_lock);
    if (!ready_unlocked()) {
        result.error.code = MEWNET_ERROR_CLOSED;
        copy_text(result.error.message, sizeof(result.error.message), g.error, "");
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    if (url == NULL || url[0] == '\0') {
        set_error("subscription url is required");
        result.error.code = MEWNET_ERROR_INVALID_ARGUMENT;
        copy_text(result.error.message, sizeof(result.error.message), g.error, "");
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    mewnet_error err = mewnet_subs_add_subscription_in_group(g.db, g.group, url, 0, NULL);
    if (err.code != MEWNET_ERROR_NONE) {
        set_error("%s", err.message);
        result.error = err;
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    int64_t id = find_subscription_by_url(url);
    if (id == 0) {
        set_error("subscription was stored but cannot be found again");
        result.error.code = MEWNET_ERROR_FAILED;
        copy_text(result.error.message, sizeof(result.error.message), g.error, "");
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    result.id = id;

    /* The subscription stays even when the first fetch fails. */
    mewnet_subs_fetch_result fetched = mewnet_subs_fetch_now(g.db, id);
    if (fetched.error.code != MEWNET_ERROR_NONE) {
        set_error("fetch: %s", fetched.error.message);
        result.error = fetched.error;
    } else {
        clear_error();
        result.error.code = MEWNET_ERROR_NONE;
    }
    pthread_mutex_unlock(&g_lock);
    return result;
}

bool RemoveMaunetSubscription(int64_t subscription_id) {
    pthread_mutex_lock(&g_lock);
    if (!ready_unlocked()) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    mewnet_error err = mewnet_subs_remove_subscription(g.db, subscription_id);
    if (err.code != MEWNET_ERROR_NONE) {
        set_error("%s", err.message);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    clear_error();
    pthread_mutex_unlock(&g_lock);
    return true;
}

bool RefreshMaunetSubscriptions(void) {
    pthread_mutex_lock(&g_lock);
    if (!ready_unlocked()) {
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    mewnet_subscription_list subs = mewnet_subs_load_subscriptions(g.db);
    if (subs.error.code != MEWNET_ERROR_NONE) {
        set_error("%s", subs.error.message);
        mewnet_subs_unload_subscriptions(subs);
        pthread_mutex_unlock(&g_lock);
        return false;
    }
    size_t ok = 0;
    size_t failed = 0;
    char last_error[MEWNET_ERROR_MAX];
    last_error[0] = '\0';
    for (size_t i = 0; i < subs.count; i++) {
        mewnet_subs_fetch_result fetched = mewnet_subs_fetch_now(g.db, subs.items[i].id);
        if (fetched.error.code == MEWNET_ERROR_NONE) {
            ok++;
        } else {
            failed++;
            snprintf(last_error, sizeof(last_error), "%s", fetched.error.message);
        }
    }
    size_t total = subs.count;
    mewnet_subs_unload_subscriptions(subs);
    if (ok > 0 || total == 0) {
        clear_error();
        pthread_mutex_unlock(&g_lock);
        return true;
    }
    set_error("all %zu subscription(s) failed: %s", failed, last_error);
    pthread_mutex_unlock(&g_lock);
    return false;
}

maunet_subscription_list LoadMaunetSubscriptions(void) {
    maunet_subscription_list result;
    memset(&result, 0, sizeof(result));
    pthread_mutex_lock(&g_lock);
    if (!ready_unlocked()) {
        result.error = MEWNET_ERROR_FAILED;
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    mewnet_subscription_list rows = mewnet_subs_load_subscriptions(g.db);
    if (rows.error.code != MEWNET_ERROR_NONE) {
        set_error("%s", rows.error.message);
        result.error = rows.error.code;
        mewnet_subs_unload_subscriptions(rows);
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    maunet_subscription_info* items = NULL;
    if (rows.count > 0) {
        items = (maunet_subscription_info*)calloc(rows.count, sizeof(maunet_subscription_info));
    }
    if (items == NULL && rows.count > 0) {
        set_error("out of memory");
        result.error = MEWNET_ERROR_FAILED;
        mewnet_subs_unload_subscriptions(rows);
        pthread_mutex_unlock(&g_lock);
        return result;
    }
    for (size_t i = 0; i < rows.count; i++) {
        const mewnet_subscription* row = &rows.items[i];
        maunet_subscription_info* info = &items[i];
        info->id = row->id;
        copy_text(info->url, sizeof(info->url), row->url, "");
        copy_text(info->name, sizeof(info->name), row->name, "");
        info->priority = row->priority;
        info->enabled = row->enabled;
        info->status = row->last_status;
        info->failure_count = row->failure_count;
        info->last_fetch_at = row->last_fetch_at;
        copy_text(info->last_error, sizeof(info->last_error), row->last_error, "");
    }
    mewnet_subs_unload_subscriptions(rows);
    result.ok = true;
    result.error = MEWNET_ERROR_NONE;
    result.items = items;
    result.count = rows.count;
    clear_error();
    pthread_mutex_unlock(&g_lock);
    return result;
}

void UnloadMaunetSubscriptions(maunet_subscription_list list) {
    free(list.items);
}

int64_t GetMaunetSubscriptionCount(void) {
    int64_t count = 0;
    pthread_mutex_lock(&g_lock);
    if (g.ready) {
        mewnet_subs_count_subscriptions(g.db, 0, &count);
    }
    pthread_mutex_unlock(&g_lock);
    return count;
}
