/*
 * mewnet-gtk - GTK4 control panel for subscription discovery.
 *
 * The UI runs on the GTK main thread. A single worker thread owns discovery:
 * it fetches subscriptions, tests proxies and asks libmewnet to route through
 * the first working node. The worker never touches GTK directly; it posts
 * log lines and the active proxy snapshot to the main loop with g_idle_add().
 */

#include "mewnet_subs.h"

#include <gtk/gtk.h>
#include <glib-unix.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_ID "org.mewnet.gtk"
#define LOG_MAX_LINES 2000
#define LOG_KEEP_LINES 1500

static volatile sig_atomic_t g_headless_stop = 0;
static volatile sig_atomic_t g_headless_switch = 0;

typedef struct App App;

typedef struct Options {
    char* db_path;
    int proxy_port;
    size_t batch;
    size_t concurrency;
    int timeout_ms;
    int fetch_interval_sec;
    int slow_interval_sec;
    int pool_target;
    int health_interval_sec;
    int health_timeout_ms;
    char* test_url;
    char* test_expected;
    char* health_url;
    char* health_expected;
    bool headless;
    int run_seconds;
    char* screenshot_path;
    bool screenshot_exit;
    GPtrArray* urls;
} Options;

struct App {
    GtkWidget* window;
    GtkWidget* local_proxy_label;
    GtkWidget* state_label;
    GtkWidget* current_name_label;
    GtkWidget* current_detail_label;
    GtkWidget* counts_label;
    GtkWidget* switch_button;
    GtkWidget* subs_list;
    GtkWidget* subs_empty_label;
    GtkWidget* subs_entry;
    GtkWidget* subs_priority;
    GtkWidget* remove_button;
    GtkWidget* log_view;
    GtkTextBuffer* log_buffer;

    Options options;

    mewnet_subs* db;      /* worker connection */
    mewnet_subs* ui_db;   /* main-thread connection for lists/counts */
    mewnet_instance* milib;
    int64_t group_id;
    int actual_port;

    GMutex lock;
    GCond cond;
    bool stop;
    bool closing;
    bool cleaned;

    int64_t pending_fetch_id; /* guarded */
    bool switch_requested;    /* guarded */

    int64_t current_id; /* guarded */
    int64_t last_switch_id;
    char current_name[MEWNET_NAME_MAX];
    char current_protocol[MEWNET_TYPE_MAX];
    char current_server[MEWNET_HOST_MAX];
    int current_port;
    unsigned current_delay;

    GArray* sub_ids; /* UI only: id for each list row */
    int64_t selected_sub_id;
    int64_t ui_sub_count;
    int64_t ui_max_sub_id;
    uint64_t ui_sub_signature;

    GThread* worker;
    guint refresh_source;

    bool headless;
    bool announced_ready;
    bool force_pool_batch;
    int health_failures;
    int64_t next_health_ms;
    int64_t next_discover_ms;
    int64_t last_pool_batch_ms;
    int64_t last_emergency_fetch_ms;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct {
    App* app;
    char* text;
} LogMessage;

static gboolean append_log_idle(gpointer data) {
    LogMessage* message = (LogMessage*)data;
    App* app = message->app;
    if (!app->closing && app->log_buffer != NULL) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(app->log_buffer, &end);
        gtk_text_buffer_insert(app->log_buffer, &end, message->text, -1);

        int lines = gtk_text_buffer_get_line_count(app->log_buffer);
        if (lines > LOG_MAX_LINES) {
            GtkTextIter start;
            GtkTextIter cut;
            gtk_text_buffer_get_start_iter(app->log_buffer, &start);
            gtk_text_buffer_get_iter_at_line(app->log_buffer, &cut, lines - LOG_KEEP_LINES);
            gtk_text_buffer_delete(app->log_buffer, &start, &cut);
        }

        if (app->log_view != NULL) {
            gtk_text_buffer_get_end_iter(app->log_buffer, &end);
            GtkTextMark* mark = gtk_text_buffer_create_mark(app->log_buffer, NULL, &end, FALSE);
            gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(app->log_view), mark);
            gtk_text_buffer_delete_mark(app->log_buffer, mark);
        }
    }
    g_free(message->text);
    g_free(message);
    return G_SOURCE_REMOVE;
}

static void log_line(App* app, const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    char stamp[32];
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);

    char* line = g_strdup_printf("%s  %s\n", stamp, text);
    g_print("%s", line);
    if (app->headless) {
        fflush(stdout);
        g_free(line);
        return;
    }

    LogMessage* message = g_new0(LogMessage, 1);
    message->app = app;
    message->text = line;
    g_idle_add(append_log_idle, message);
}

static void app_wake(App* app) {
    g_mutex_lock(&app->lock);
    g_cond_signal(&app->cond);
    g_mutex_unlock(&app->lock);
}

static bool app_is_stopped(App* app) {
    g_mutex_lock(&app->lock);
    bool stop = app->stop;
    g_mutex_unlock(&app->lock);
    return stop;
}

/* Wait up to `ms`, but return early for stop/new-subscription/switch. */
static void app_wait(App* app, int ms) {
    if (ms <= 0) {
        return;
    }
    gint64 deadline = g_get_monotonic_time() + (gint64)ms * 1000;
    g_mutex_lock(&app->lock);
    while (!app->stop && app->pending_fetch_id == 0 && !app->switch_requested) {
        if (!g_cond_wait_until(&app->cond, &app->lock, deadline)) {
            break;
        }
    }
    g_mutex_unlock(&app->lock);
}

static int64_t take_pending_fetch(App* app) {
    g_mutex_lock(&app->lock);
    int64_t id = app->pending_fetch_id;
    app->pending_fetch_id = 0;
    g_mutex_unlock(&app->lock);
    return id;
}

static bool take_switch_request(App* app) {
    g_mutex_lock(&app->lock);
    bool requested = app->switch_requested;
    app->switch_requested = false;
    g_mutex_unlock(&app->lock);
    return requested;
}

/* ------------------------------------------------------------------ */
/* current proxy snapshot                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    App* app;
    int64_t id;
    char name[MEWNET_NAME_MAX];
    char protocol[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port;
    unsigned delay;
} CurrentMessage;

static gboolean set_current_idle(gpointer data) {
    CurrentMessage* message = (CurrentMessage*)data;
    App* app = message->app;
    if (!app->closing && app->current_name_label != NULL) {
        const char* title = message->name[0] != '\0' ? message->name : "unnamed";
        char* name_text = g_strdup_printf("Currently using: %s", title);
        gtk_label_set_text(GTK_LABEL(app->current_name_label), name_text);
        g_free(name_text);

        GString* detail = g_string_new(NULL);
        if (message->protocol[0] != '\0') {
            g_string_append(detail, message->protocol);
        }
        if (message->server[0] != '\0') {
            if (detail->len > 0) {
                g_string_append(detail, " · ");
            }
            g_string_append_printf(detail, "%s:%d", message->server, message->port);
        }
        if (message->delay > 0) {
            if (detail->len > 0) {
                g_string_append(detail, " · ");
            }
            g_string_append_printf(detail, "%u ms", message->delay);
        }
        if (detail->len == 0) {
            g_string_append(detail, "waiting for health check");
        }
        gtk_label_set_text(GTK_LABEL(app->current_detail_label), detail->str);
        g_string_free(detail, TRUE);
    }
    g_free(message);
    return G_SOURCE_REMOVE;
}

static void publish_current(App* app, const mewnet_subs_proxy* proxy) {
    g_mutex_lock(&app->lock);
    app->current_id = proxy->id;
    snprintf(app->current_name, sizeof(app->current_name), "%s", proxy->name);
    snprintf(app->current_protocol, sizeof(app->current_protocol), "%s", proxy->protocol);
    snprintf(app->current_server, sizeof(app->current_server), "%s", proxy->server);
    app->current_port = proxy->port;
    app->current_delay = proxy->last_delay_ms;
    g_mutex_unlock(&app->lock);
    if (app->headless) {
        return;
    }

    CurrentMessage* message = g_new0(CurrentMessage, 1);
    message->app = app;
    message->id = proxy->id;
    snprintf(message->name, sizeof(message->name), "%s", proxy->name);
    snprintf(message->protocol, sizeof(message->protocol), "%s", proxy->protocol);
    snprintf(message->server, sizeof(message->server), "%s", proxy->server);
    message->port = proxy->port;
    message->delay = proxy->last_delay_ms;
    g_idle_add(set_current_idle, message);
}

/* ------------------------------------------------------------------ */
/* discovery worker                                                    */
/* ------------------------------------------------------------------ */

static size_t load_subscriptions(App* app, mewnet_subs* db, mewnet_subscription_list* out) {
    *out = mewnet_subs_load_subscriptions_in_group(db, app->group_id);
    if (out->error.code != MEWNET_ERROR_NONE) {
        return 0;
    }
    return out->count;
}

static int64_t max_last_fetch(App* app) {
    mewnet_subscription_list list;
    int64_t last = 0;
    if (load_subscriptions(app, app->db, &list) == 0) {
        mewnet_subs_unload_subscriptions(list);
        return 0;
    }
    for (size_t i = 0; i < list.count; i++) {
        if (list.items[i].last_fetch_at > last) {
            last = list.items[i].last_fetch_at;
        }
    }
    mewnet_subs_unload_subscriptions(list);
    return last;
}

/* Fetch the subscription that was refreshed the longest time ago. */
static bool fetch_next_subscription(App* app, bool emergency) {
    mewnet_subscription_list list;
    if (load_subscriptions(app, app->db, &list) == 0) {
        mewnet_subs_unload_subscriptions(list);
        return false;
    }

    int64_t best_id = 0;
    int64_t best_last = 0;
    for (size_t i = 0; i < list.count; i++) {
        if (!list.items[i].enabled) {
            continue;
        }
        int64_t last = list.items[i].last_fetch_at;
        if (best_id == 0 || last < best_last) {
            best_id = list.items[i].id;
            best_last = last;
        }
    }
    mewnet_subs_unload_subscriptions(list);
    if (best_id == 0) {
        return false;
    }

    mewnet_subs_fetch_result fetched = mewnet_subs_fetch_now(app->db, best_id);
    if (fetched.error.code != MEWNET_ERROR_NONE) {
        log_line(app, "FETCH FAILED id=%lld%s%s", (long long)best_id,
                 emergency ? " (emergency)" : "", fetched.error.message);
        return false;
    }
    log_line(app, "FETCH id=%lld status=%d added=%zu updated=%zu removed=%zu%s",
             (long long)fetched.subscription_id, fetched.http_status,
             fetched.added, fetched.updated, fetched.removed,
             emergency ? " (emergency)" : "");
    return true;
}

static bool fetch_subscription(App* app, int64_t id) {
    if (id == 0) {
        return false;
    }
    mewnet_subs_fetch_result fetched = mewnet_subs_fetch_now(app->db, id);
    if (fetched.error.code != MEWNET_ERROR_NONE) {
        log_line(app, "FETCH FAILED id=%lld: %s", (long long)id, fetched.error.message);
        return false;
    }
    log_line(app, "FETCH id=%lld status=%d added=%zu updated=%zu removed=%zu",
             (long long)fetched.subscription_id, fetched.http_status,
             fetched.added, fetched.updated, fetched.removed);
    return true;
}

static bool route_proxy(App* app, const mewnet_subs_proxy* proxy, bool announce_ready) {
    mewnet_status_info before = mewnet_get_status(app->milib);
    char previous[MEWNET_NAME_MAX];
    snprintf(previous, sizeof(previous), "%s", before.selected);

    mewnet_add_options options = mewnet_add_options_default();
    options.select_proxy = true;
    mewnet_error_code code = mewnet_add_proxy(app->milib, proxy->link, &options);
    if (code != MEWNET_ERROR_NONE) {
        log_line(app, "ROUTE FAILED id=%lld: code %d", (long long)proxy->id, (int)code);
        return false;
    }

    mewnet_status_info status = mewnet_get_status(app->milib);
    if (previous[0] != '\0' && strcmp(previous, status.selected) != 0 &&
        strcmp(previous, "DIRECT") != 0) {
        mewnet_remove_proxy(app->milib, previous);
    }
    mewnet_subs_proxy display = *proxy;
    if (status.selected[0] != '\0') {
        snprintf(display.name, sizeof(display.name), "%s", status.selected);
    }
    publish_current(app, &display);
    log_line(app, "ACTIVE id=%lld name=%s delay=%u",
             (long long)proxy->id,
             display.name[0] != '\0' ? display.name : "(manual)",
             proxy->last_delay_ms);
    if (announce_ready) {
        log_line(app, "READY local=http://127.0.0.1:%d", app->actual_port);
    }
    return true;
}

static mewnet_subs_proxy get_proxy_snapshot(mewnet_subs* db, int64_t id) {
    mewnet_subs_proxy proxy;
    memset(&proxy, 0, sizeof(proxy));
    mewnet_subs_get_proxy(db, id, &proxy);
    return proxy;
}

static bool route_by_id(App* app, int64_t id, bool announce_ready) {
    mewnet_subs_proxy proxy = get_proxy_snapshot(app->db, id);
    if (proxy.id == 0) {
        return false;
    }
    return route_proxy(app, &proxy, announce_ready);
}

static bool health_check_active(App* app);
static void fail_current(App* app, const char* reason);

/* Pick the next alive node, route through it and verify it immediately.
 * A stale "alive" entry is therefore retired during failover instead of
 * costing another full health-check interval. */
static bool switch_next_alive(App* app, bool verify) {
    int64_t current_id;
    g_mutex_lock(&app->lock);
    current_id = app->current_id;
    g_mutex_unlock(&app->lock);

    int64_t alive = 0;
    mewnet_subs_count_alive(app->db, app->group_id, &alive);
    if (alive == 0) {
        return false;
    }

    int64_t exclude = current_id;
    for (int64_t attempt = 0; attempt < alive; attempt++) {
        mewnet_subs_proxy proxy;
        mewnet_error err = mewnet_subs_next_alive(app->db, app->group_id, exclude, &proxy);
        if (err.code == MEWNET_ERROR_NOT_FOUND) {
            break;
        }
        if (err.code != MEWNET_ERROR_NONE) {
            log_line(app, "SWITCH FAILED: %s", err.message);
            break;
        }
        exclude = proxy.id;

        if (!route_proxy(app, &proxy, !app->announced_ready)) {
            mewnet_subs_record_test(app->db, proxy.id, false, 0, "route failed");
            continue;
        }
        app->announced_ready = true;
        if (!verify || health_check_active(app)) {
            if (current_id != 0) {
                log_line(app, "SWITCH id=%lld name=%s", (long long)proxy.id,
                         proxy.name[0] != '\0' ? proxy.name : "(manual)");
            }
            return true;
        }
        log_line(app, "SWITCH candidate id=%lld failed verification", (long long)proxy.id);
        fail_current(app, "failover verification failed");
    }

    log_line(app, "SWITCH failed: no verified working proxy");
    return false;
}

/* Manual switch: choose randomly among the best alive candidates, excluding
 * the current node and the node used by the previous manual switch. */
static bool switch_random_best(App* app) {
    int64_t current_id;
    g_mutex_lock(&app->lock);
    current_id = app->current_id;
    g_mutex_unlock(&app->lock);

    int64_t alive = 0;
    mewnet_subs_count_alive(app->db, app->group_id, &alive);
    if (alive <= 1) {
        return false;
    }

    int64_t exclude = current_id;
    int64_t avoid = app->last_switch_id;
    for (int64_t attempt = 0; attempt < alive; attempt++) {
        mewnet_subs_proxy proxy;
        mewnet_error err = mewnet_subs_random_best_alive(
            app->db, app->group_id, exclude, avoid, 5, &proxy);
        if (err.code == MEWNET_ERROR_NOT_FOUND && avoid != 0) {
            avoid = 0;
            err = mewnet_subs_random_best_alive(
                app->db, app->group_id, exclude, avoid, 5, &proxy);
        }
        if (err.code == MEWNET_ERROR_NOT_FOUND) {
            break;
        }
        if (err.code != MEWNET_ERROR_NONE) {
            log_line(app, "SWITCH FAILED: %s", err.message);
            break;
        }

        if (!route_proxy(app, &proxy, !app->announced_ready)) {
            mewnet_subs_record_test(app->db, proxy.id, false, 0, "route failed");
            exclude = proxy.id;
            continue;
        }
        app->announced_ready = true;
        if (health_check_active(app)) {
            int64_t previous = current_id;
            app->last_switch_id = previous;
            log_line(app, "SWITCH random id=%lld name=%s", (long long)proxy.id,
                     proxy.name[0] != '\0' ? proxy.name : "(manual)");
            return true;
        }
        log_line(app, "SWITCH candidate id=%lld failed verification", (long long)proxy.id);
        fail_current(app, "switch verification failed");
        exclude = 0;
        avoid = 0;
    }

    log_line(app, "SWITCH failed: no verified working proxy");
    return false;
}

static int64_t count_subscriptions_worker(App* app) {
    mewnet_subscription_list list;
    size_t count = load_subscriptions(app, app->db, &list);
    mewnet_subs_unload_subscriptions(list);
    return (int64_t)count;
}

static uint64_t fnv_mix(uint64_t hash, const void* data, size_t size) {
    const uint8_t* bytes = (const uint8_t*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

static int64_t count_subscriptions_ui(App* app, int64_t* max_id, uint64_t* signature) {
    mewnet_subscription_list list = mewnet_subs_load_subscriptions_in_group(app->ui_db, app->group_id);
    if (list.error.code != MEWNET_ERROR_NONE) {
        return 0;
    }
    int64_t count = (int64_t)list.count;
    int64_t last_id = 0;
    uint64_t hash = 1469598103934665603ULL;
    for (size_t i = 0; i < list.count; i++) {
        mewnet_subscription* sub = &list.items[i];
        if (sub->id > last_id) {
            last_id = sub->id;
        }
        hash = fnv_mix(hash, &sub->id, sizeof(sub->id));
        hash = fnv_mix(hash, &sub->last_fetch_at, sizeof(sub->last_fetch_at));
        hash = fnv_mix(hash, &sub->last_status, sizeof(sub->last_status));
        hash = fnv_mix(hash, &sub->failure_count, sizeof(sub->failure_count));
        hash = fnv_mix(hash, sub->last_error, strlen(sub->last_error));
    }
    mewnet_subs_unload_subscriptions(list);
    if (max_id != NULL) {
        *max_id = last_id;
    }
    if (signature != NULL) {
        *signature = hash;
    }
    return count;
}

static int64_t count_proxies_worker(App* app) {
    int64_t count = 0;
    mewnet_subs_count_proxies(app->db, app->group_id, &count);
    return count;
}

static void clear_current(App* app) {
    g_mutex_lock(&app->lock);
    app->current_id = 0;
    app->current_name[0] = '\0';
    g_mutex_unlock(&app->lock);
}

static bool health_check_active(App* app) {
    char name[MEWNET_NAME_MAX];
    g_mutex_lock(&app->lock);
    snprintf(name, sizeof(name), "%s", app->current_name);
    g_mutex_unlock(&app->lock);
    if (name[0] == '\0') {
        return false;
    }

    mewnet_delay_options options = mewnet_delay_options_default();
    options.url = app->options.health_url;
    options.timeout_ms = app->options.health_timeout_ms;
    options.expected = app->options.health_expected;
    mewnet_delay_result result = mewnet_test_delay(app->milib, name, &options);
    return result.ok;
}

static void fail_current(App* app, const char* reason) {
    int64_t id;
    char name[MEWNET_NAME_MAX];
    g_mutex_lock(&app->lock);
    id = app->current_id;
    snprintf(name, sizeof(name), "%s", app->current_name);
    g_mutex_unlock(&app->lock);
    if (id == 0) {
        return;
    }
    mewnet_subs_record_test(app->db, id, false, 0, reason);
    log_line(app, "HEALTH FAIL id=%lld name=%s", (long long)id,
             name[0] != '\0' ? name : "(unknown)");
    clear_current(app);
}

static bool run_batch(App* app, const char* reason, bool route_found) {
    mewnet_subs_batch_test_result tested =
        mewnet_subs_test_batch(app->db, app->milib, app->group_id,
                               app->options.batch, app->options.concurrency);
    if (tested.error.code != MEWNET_ERROR_NONE) {
        log_line(app, "BATCH FAILED reason=%s: %s", reason, tested.error.message);
        return false;
    }
    log_line(app, "BATCH reason=%s tested=%zu alive=%zu", reason,
             tested.tested, tested.alive);
    if (tested.alive > 0 && tested.first_alive_proxy_id != 0) {
        if (!route_found) {
            log_line(app, "DISCOVERED id=%lld", (long long)tested.first_alive_proxy_id);
            return true;
        }
        if (route_by_id(app, tested.first_alive_proxy_id, !app->announced_ready)) {
            app->announced_ready = true;
            return true;
        }
    }
    return false;
}

static gpointer worker_main(gpointer data) {
    App* app = (App*)data;
    int64_t last_fetch_ms = max_last_fetch(app);
    int64_t started = now_ms();
    bool waiting_logged = false;
    int64_t last_subs = -1;
    app->next_health_ms = started + 1500;
    app->next_discover_ms = started;

    log_line(app, "DISCOVERY start batch=%zu concurrency=%zu timeout=%dms pool_target=%d",
             app->options.batch, app->options.concurrency,
             app->options.timeout_ms, app->options.pool_target);

    while (!app_is_stopped(app)) {
        if (app->headless) {
            if (g_headless_stop) {
                g_mutex_lock(&app->lock);
                app->stop = true;
                g_mutex_unlock(&app->lock);
            }
            if (g_headless_switch) {
                g_headless_switch = 0;
                g_mutex_lock(&app->lock);
                app->switch_requested = true;
                g_mutex_unlock(&app->lock);
            }
        }
        if (app->options.run_seconds > 0 &&
            now_ms() - started >= (int64_t)app->options.run_seconds * 1000) {
            log_line(app, "RUN complete seconds=%d", app->options.run_seconds);
            break;
        }

        int64_t pending = take_pending_fetch(app);
        if (pending != 0) {
            if (fetch_subscription(app, pending)) {
                last_fetch_ms = now_ms();
            }
            continue;
        }

        int64_t subs = count_subscriptions_worker(app);
        if (subs != last_subs) {
            last_subs = subs;
            log_line(app, "SUBS n=%lld", (long long)subs);
        }

        if (subs == 0) {
            if (!waiting_logged) {
                log_line(app, "WAIT no subscriptions yet - add a URL");
                waiting_logged = true;
            }
            app_wait(app, 500);
            continue;
        }
        waiting_logged = false;

        int64_t alive = 0;
        int64_t untested = 0;
        int64_t proxies = count_proxies_worker(app);
        mewnet_subs_count_alive(app->db, app->group_id, &alive);
        mewnet_subs_count_untested(app->db, app->group_id, &untested);
        int64_t now = now_ms();

        /* No working proxy: run wide, fast batches until one is found. */
        if (alive == 0) {
            if (proxies == 0) {
                if (fetch_next_subscription(app, true)) {
                    last_fetch_ms = now_ms();
                } else {
                    app_wait(app, 2000);
                }
                continue;
            }

            if (run_batch(app, "fast", true)) {
                app->next_health_ms = now_ms() + 1500;
                app->next_discover_ms = now_ms() + 500;
                continue;
            }

            /* Everything we have is dead. Pull the next source, but do not
             * hammer subscription servers in a tight loop. */
            if (now - app->last_emergency_fetch_ms >= 10000) {
                app->last_emergency_fetch_ms = now;
                if (fetch_next_subscription(app, true)) {
                    last_fetch_ms = now_ms();
                } else {
                    app_wait(app, 1500);
                }
            } else {
                app_wait(app, 1000);
            }
            continue;
        }

        /* User asked for another proxy. */
        if (take_switch_request(app)) {
            log_line(app, "SWITCH requested");
            if (!switch_random_best(app)) {
                log_line(app, "SWITCH no other working proxy; looking for candidates");
                if (run_batch(app, "switch", true)) {
                    app->announced_ready = true;
                } else if (now - app->last_emergency_fetch_ms >= 30000) {
                    app->last_emergency_fetch_ms = now;
                    if (fetch_next_subscription(app, true)) {
                        last_fetch_ms = now;
                    }
                }
            }
            if (alive < app->options.pool_target &&
                now - app->last_pool_batch_ms >= 10000) {
                app->force_pool_batch = true;
            }
            app->next_health_ms = now_ms() + 1500;
            app->next_discover_ms = now_ms() + 500;
            continue;
        }

        int64_t current_id;
        g_mutex_lock(&app->lock);
        current_id = app->current_id;
        g_mutex_unlock(&app->lock);

        if (current_id == 0) {
            if (switch_next_alive(app, false)) {
                app->announced_ready = true;
            }
            app->next_health_ms = now_ms() + 1500;
            continue;
        }

        mewnet_subs_proxy current = get_proxy_snapshot(app->db, current_id);
        if (current.id == 0) {
            clear_current(app);
            continue;
        }
        if (!current.alive) {
            log_line(app, "ACTIVE id=%lld is dead; failing over", (long long)current_id);
            if (!switch_next_alive(app, true)) {
                clear_current(app);
            }
            app->next_health_ms = now_ms() + 1500;
            continue;
        }

        /* End-to-end health check of the active proxy. Two consecutive
         * failures are required before it is marked dead, which avoids
         * flapping on a single lost packet. */
        if (now >= app->next_health_ms) {
            if (health_check_active(app)) {
                app->health_failures = 0;
                app->next_health_ms = now + (int64_t)app->options.health_interval_sec * 1000;
            } else {
                app->health_failures++;
                log_line(app, "HEALTH FAIL try=%d/%d", app->health_failures, 2);
                if (app->health_failures >= 2) {
                    fail_current(app, "health check failed");
                    app->health_failures = 0;
                    if (!switch_next_alive(app, true)) {
                        clear_current(app);
                    }
                    app->next_health_ms = now + 1500;
                } else {
                    app->next_health_ms = now + 800;
                }
            }
            continue;
        }

        /* Refresh one source per fetch interval. */
        int64_t fetch_interval_ms = (int64_t)app->options.fetch_interval_sec * 1000;
        if (last_fetch_ms == 0 || now - last_fetch_ms >= fetch_interval_ms) {
            if (fetch_next_subscription(app, false)) {
                last_fetch_ms = now_ms();
            } else {
                app_wait(app, 5000);
            }
            continue;
        }

        /* Grow the pool after a switch or whenever it is below target. */
        if (app->force_pool_batch) {
            app->force_pool_batch = false;
            app->last_pool_batch_ms = now;
            run_batch(app, "pool", false);
            app->next_discover_ms = now_ms() + 1500;
            continue;
        }

        /* Slow background discovery: never retest a healthy node. Prefer
         * untested configs, then dead ones. */
        if (now >= app->next_discover_ms) {
            int64_t interval = alive < app->options.pool_target
                                   ? 2000
                                   : (int64_t)app->options.slow_interval_sec * 1000;
            app->next_discover_ms = now + interval;

            mewnet_subs_test_result result = mewnet_subs_test_candidate_now(
                app->db, app->milib, MEWNET_SUBS_CANDIDATE_UNTESTED);
            if (result.error.code == MEWNET_ERROR_NOT_FOUND) {
                result = mewnet_subs_test_candidate_now(
                    app->db, app->milib, MEWNET_SUBS_CANDIDATE_NOT_ALIVE);
            }

            if (result.error.code == MEWNET_ERROR_NONE) {
                log_line(app, "TEST id=%lld alive=%d delay=%u rating=%d->%d",
                         (long long)result.proxy_id, result.alive ? 1 : 0,
                         result.delay_ms, result.rating_before, result.rating_after);
                if (result.alive) {
                    log_line(app, "DISCOVERED id=%lld delay=%u",
                             (long long)result.proxy_id, result.delay_ms);
                }
            } else if (result.error.code != MEWNET_ERROR_NOT_FOUND) {
                log_line(app, "TEST FAILED: %s", result.error.message);
            }
            continue;
        }

        app_wait(app, 500);
    }

    log_line(app, "DISCOVERY stopped");
    return NULL;
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static void set_margins(GtkWidget* widget, int all) {
    gtk_widget_set_margin_start(widget, all);
    gtk_widget_set_margin_end(widget, all);
    gtk_widget_set_margin_top(widget, all);
    gtk_widget_set_margin_bottom(widget, all);
}

static void show_error(App* app, const char* title, const char* message) {
    log_line(app, "%s: %s", title, message);
    if (app->headless || app->window == NULL) {
        return;
    }
    GtkAlertDialog* dialog = gtk_alert_dialog_new("%s", title);
    gtk_alert_dialog_set_detail(dialog, message);
    gtk_alert_dialog_show(dialog, GTK_WINDOW(app->window));
    g_object_unref(dialog);
}

static void update_local_proxy_label(App* app) {
    char* text = g_strdup_printf("Local proxy:  http://127.0.0.1:%d", app->actual_port);
    gtk_label_set_text(GTK_LABEL(app->local_proxy_label), text);
    g_free(text);
}

static void reload_subscriptions(App* app) {
    mewnet_subscription_list list =
        mewnet_subs_load_subscriptions_in_group(app->ui_db, app->group_id);
    if (list.error.code != MEWNET_ERROR_NONE) {
        return;
    }

    int64_t selected = app->selected_sub_id;
    int selected_index = -1;
    int index = 0;

    gtk_list_box_remove_all(GTK_LIST_BOX(app->subs_list));
    g_array_set_size(app->sub_ids, 0);

    for (size_t i = 0; i < list.count; i++) {
        mewnet_subscription* sub = &list.items[i];
        g_array_append_val(app->sub_ids, sub->id);

        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        set_margins(box, 6);

        GtkWidget* url = gtk_label_new(sub->url);
        gtk_label_set_xalign(GTK_LABEL(url), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(url), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(url), 52);
        gtk_widget_set_tooltip_text(url, sub->url);
        gtk_box_append(GTK_BOX(box), url);

        char status[512];
        if (sub->last_fetch_at == 0) {
            snprintf(status, sizeof(status), "not fetched");
        } else if (sub->last_error[0] != '\0') {
            snprintf(status, sizeof(status), "error: %.100s", sub->last_error);
        } else {
            snprintf(status, sizeof(status), "HTTP %d", sub->last_status);
        }
        char* meta = g_strdup_printf("priority %d · %s", sub->priority, status);
        GtkWidget* meta_label = gtk_label_new(meta);
        gtk_label_set_xalign(GTK_LABEL(meta_label), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(meta_label), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(meta_label, "hint");
        gtk_box_append(GTK_BOX(box), meta_label);
        g_free(meta);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(app->subs_list), row);

        if (sub->id == selected) {
            selected_index = index;
        }
        index++;
    }

    if (selected_index >= 0) {
        GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(app->subs_list), selected_index);
        if (row != NULL) {
            gtk_list_box_select_row(GTK_LIST_BOX(app->subs_list), row);
            app->selected_sub_id = selected;
        }
    } else {
        app->selected_sub_id = 0;
        gtk_list_box_unselect_all(GTK_LIST_BOX(app->subs_list));
    }

    gtk_widget_set_visible(app->subs_empty_label, list.count == 0);
    app->ui_sub_count = (int64_t)list.count;
    mewnet_subs_unload_subscriptions(list);
}

static void on_sub_selection_changed(GtkListBox* box, GtkListBoxRow* row, gpointer user) {
    (void)box;
    App* app = (App*)user;
    if (row == NULL) {
        app->selected_sub_id = 0;
        return;
    }
    int index = gtk_list_box_row_get_index(row);
    if (index >= 0 && index < (int)app->sub_ids->len) {
        app->selected_sub_id = g_array_index(app->sub_ids, int64_t, index);
    }
}

static void on_add_subscription(GtkButton* button, gpointer user) {
    (void)button;
    App* app = (App*)user;
    const char* url = gtk_editable_get_text(GTK_EDITABLE(app->subs_entry));
    if (url == NULL || url[0] == '\0') {
        log_line(app, "ADD: enter a subscription URL");
        return;
    }

    int priority = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(app->subs_priority));
    mewnet_error err = mewnet_subs_add_subscription_in_group(app->ui_db, app->group_id,
                                                             url, priority, NULL);
    if (err.code != MEWNET_ERROR_NONE) {
        log_line(app, "ADD FAILED: %s", err.message);
        return;
    }

    gtk_editable_set_text(GTK_EDITABLE(app->subs_entry), "");

    mewnet_subscription_list list =
        mewnet_subs_load_subscriptions_in_group(app->ui_db, app->group_id);
    int64_t new_id = 0;
    if (list.error.code == MEWNET_ERROR_NONE) {
        for (size_t i = 0; i < list.count; i++) {
            if (strcmp(list.items[i].url, url) == 0) {
                new_id = list.items[i].id;
            }
        }
        mewnet_subs_unload_subscriptions(list);
    }

    if (new_id != 0) {
        g_mutex_lock(&app->lock);
        app->pending_fetch_id = new_id;
        g_mutex_unlock(&app->lock);
        app_wake(app);
    }

    log_line(app, "ADD ok id=%lld priority=%d", (long long)new_id, priority);
    reload_subscriptions(app);
}

static void on_remove_subscription(GtkButton* button, gpointer user) {
    (void)button;
    App* app = (App*)user;
    if (app->selected_sub_id == 0) {
        log_line(app, "REMOVE: select a subscription first");
        return;
    }
    int64_t id = app->selected_sub_id;
    mewnet_error err = mewnet_subs_remove_subscription(app->ui_db, id);
    if (err.code != MEWNET_ERROR_NONE) {
        log_line(app, "REMOVE FAILED: %s", err.message);
        return;
    }
    log_line(app, "REMOVE ok id=%lld", (long long)id);
    app->selected_sub_id = 0;
    reload_subscriptions(app);
    app_wake(app);
}

static void on_switch_proxy(GtkButton* button, gpointer user) {
    (void)button;
    App* app = (App*)user;
    g_mutex_lock(&app->lock);
    app->switch_requested = true;
    g_mutex_unlock(&app->lock);
    app_wake(app);
}

static gboolean refresh_tick(gpointer user) {
    App* app = (App*)user;
    if (app->closing) {
        return G_SOURCE_REMOVE;
    }

    int64_t max_id = 0;
    uint64_t signature = 0;
    int64_t subs = count_subscriptions_ui(app, &max_id, &signature);
    if (signature != app->ui_sub_signature || max_id != app->ui_max_sub_id) {
        app->ui_sub_signature = signature;
        app->ui_max_sub_id = max_id;
        reload_subscriptions(app);
    }

    int64_t proxies = 0;
    int64_t alive = 0;
    int64_t untested = 0;
    mewnet_subs_count_proxies(app->ui_db, app->group_id, &proxies);
    mewnet_subs_count_alive(app->ui_db, app->group_id, &alive);
    mewnet_subs_count_untested(app->ui_db, app->group_id, &untested);

    const char* state;
    if (subs == 0) {
        state = "Mode: waiting for subscriptions";
    } else if (alive > 0) {
        state = "Mode: normal discovery (working proxy available)";
    } else {
        state = "Mode: fast discovery (no working proxy)";
    }
    gtk_label_set_text(GTK_LABEL(app->state_label), state);

    char* counts = g_strdup_printf("subscriptions: %lld · proxies: %lld · alive: %lld · untested: %lld",
                                   (long long)subs, (long long)proxies,
                                   (long long)alive, (long long)untested);
    gtk_label_set_text(GTK_LABEL(app->counts_label), counts);
    g_free(counts);

    update_local_proxy_label(app);
    return G_SOURCE_CONTINUE;
}

static GtkWidget* make_section_title(const char* text) {
    GtkWidget* label = gtk_label_new(NULL);
    char* markup = g_strdup_printf("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    return label;
}

static void build_subscriptions_panel(App* app, GtkWidget* parent) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(box, 12);

    gtk_box_append(GTK_BOX(box), make_section_title("Subscriptions"));

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(scroller, "subs-scroll");

    app->subs_list = gtk_list_box_new();
    gtk_widget_add_css_class(app->subs_list, "subs-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->subs_list), GTK_SELECTION_SINGLE);
    g_signal_connect(app->subs_list, "row-selected",
                     G_CALLBACK(on_sub_selection_changed), app);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app->subs_list);
    gtk_box_append(GTK_BOX(box), scroller);

    app->subs_empty_label = gtk_label_new("No subscriptions yet. Add a URL below.");
    gtk_label_set_xalign(GTK_LABEL(app->subs_empty_label), 0.0f);
    gtk_widget_add_css_class(app->subs_empty_label, "hint");
    gtk_box_append(GTK_BOX(box), app->subs_empty_label);

    GtkWidget* entry_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->subs_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->subs_entry), "https://example.com/sub");
    gtk_widget_set_hexpand(app->subs_entry, TRUE);
    GtkWidget* add_button = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(add_button, "suggested-action");
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_subscription), app);
    gtk_box_append(GTK_BOX(entry_row), app->subs_entry);
    gtk_box_append(GTK_BOX(entry_row), add_button);
    gtk_box_append(GTK_BOX(box), entry_row);

    GtkWidget* action_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget* priority_label = gtk_label_new("Priority");
    app->subs_priority = gtk_spin_button_new_with_range(-1000, 1000, 10);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(app->subs_priority), 100);
    gtk_box_append(GTK_BOX(action_row), priority_label);
    gtk_box_append(GTK_BOX(action_row), app->subs_priority);

    GtkWidget* spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(action_row), spacer);

    app->remove_button = gtk_button_new_with_label("Remove");
    g_signal_connect(app->remove_button, "clicked", G_CALLBACK(on_remove_subscription), app);
    gtk_box_append(GTK_BOX(action_row), app->remove_button);
    gtk_box_append(GTK_BOX(box), action_row);

    GtkWidget* hint = gtk_label_new("A new subscription is fetched immediately.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_add_css_class(hint, "hint");
    gtk_box_append(GTK_BOX(box), hint);

    gtk_paned_set_start_child(GTK_PANED(parent), box);
}

static void build_log_panel(App* app, GtkWidget* parent) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(box, 12);

    gtk_box_append(GTK_BOX(box), make_section_title("Discovery log"));

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(scroller, "log-scroll");

    app->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->log_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(app->log_view, "log-view");
    set_margins(app->log_view, 6);

    app->log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->log_view));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app->log_view);
    gtk_box_append(GTK_BOX(box), scroller);

    gtk_paned_set_end_child(GTK_PANED(parent), box);
}

static GtkWidget* build_status_panel(App* app) {
    GtkWidget* frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(frame, "card");
    gtk_widget_set_margin_start(frame, 16);
    gtk_widget_set_margin_end(frame, 16);
    gtk_widget_set_margin_top(frame, 12);
    gtk_widget_set_margin_bottom(frame, 6);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    set_margins(grid, 16);

    app->local_proxy_label = gtk_label_new("Local proxy:  ...");
    gtk_label_set_xalign(GTK_LABEL(app->local_proxy_label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(app->local_proxy_label), TRUE);
    gtk_widget_add_css_class(app->local_proxy_label, "local-proxy");
    gtk_widget_set_hexpand(app->local_proxy_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->local_proxy_label, 0, 0, 1, 1);

    app->state_label = gtk_label_new("Mode: starting");
    gtk_label_set_xalign(GTK_LABEL(app->state_label), 0.0f);
    gtk_widget_add_css_class(app->state_label, "state");
    gtk_grid_attach(GTK_GRID(grid), app->state_label, 1, 0, 1, 1);

    app->current_name_label = gtk_label_new("Currently using: none");
    gtk_label_set_xalign(GTK_LABEL(app->current_name_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app->current_name_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(app->current_name_label, "current-name");
    gtk_widget_set_hexpand(app->current_name_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->current_name_label, 0, 1, 1, 1);

    app->switch_button = gtk_button_new_with_label("Change proxy");
    gtk_widget_add_css_class(app->switch_button, "suggested-action");
    g_signal_connect(app->switch_button, "clicked", G_CALLBACK(on_switch_proxy), app);
    gtk_widget_set_halign(app->switch_button, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), app->switch_button, 1, 1, 1, 1);

    app->current_detail_label = gtk_label_new("the first working proxy will be selected automatically");
    gtk_label_set_xalign(GTK_LABEL(app->current_detail_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app->current_detail_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(app->current_detail_label, "hint");
    gtk_grid_attach(GTK_GRID(grid), app->current_detail_label, 0, 2, 1, 1);

    app->counts_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->counts_label), 1.0f);
    gtk_widget_add_css_class(app->counts_label, "hint");
    gtk_grid_attach(GTK_GRID(grid), app->counts_label, 1, 2, 1, 1);

    gtk_box_append(GTK_BOX(frame), grid);
    return frame;
}

static void install_css(void) {
    static const char* css =
        ".card {"
        "  background: alpha(@theme_fg_color, 0.045);"
        "  border: 1px solid alpha(@borders, 0.55);"
        "  border-radius: 14px;"
        "}"
        ".local-proxy { font-family: monospace; font-size: 15px; font-weight: 600; }"
        ".current-name { font-size: 15px; font-weight: 700; }"
        ".state { font-weight: 700; }"
        ".log-view { font-family: monospace; font-size: 12px; }"
        ".hint { opacity: 0.62; font-size: 12px; }"
        ".subs-scroll, .log-scroll {"
        "  border-radius: 12px;"
        "  border: 1px solid alpha(@borders, 0.55);"
        "  background: alpha(@theme_fg_color, 0.025);"
        "}"
        ".subs-list { background: transparent; }"
        ".subs-list > row { padding: 10px 12px; border-bottom: 1px solid alpha(@borders, 0.45); }"
        ".subs-list > row:last-child { border-bottom: none; }"
        ".subs-list > row:selected {"
        "  background: alpha(@theme_selected_bg_color, 0.18);"
        "  color: @theme_fg_color;"
        "}"
        "button.suggested-action { border-radius: 10px; font-weight: 600; }"
        "entry { border-radius: 10px; }"
        "spinbutton { border-radius: 10px; }";
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    GdkDisplay* display = gdk_display_get_default();
    if (display != NULL) {
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void cleanup(App* app) {
    if (app->cleaned) {
        return;
    }
    app->cleaned = true;
    app->closing = true;

    if (app->refresh_source != 0) {
        g_source_remove(app->refresh_source);
        app->refresh_source = 0;
    }

    g_mutex_lock(&app->lock);
    app->stop = true;
    g_cond_broadcast(&app->cond);
    g_mutex_unlock(&app->lock);

    if (app->worker != NULL) {
        g_thread_join(app->worker);
        app->worker = NULL;
    }
    if (app->milib != NULL) {
        mewnet_close(app->milib);
        app->milib = NULL;
    }
    if (app->db != NULL) {
        mewnet_subs_close(app->db);
        app->db = NULL;
    }
    if (app->ui_db != NULL) {
        mewnet_subs_close(app->ui_db);
        app->ui_db = NULL;
    }
}

static gboolean on_close_request(GtkWindow* window, gpointer user) {
    (void)window;
    App* app = (App*)user;
    cleanup(app);
    return FALSE;
}

static void on_mihomo_log(mewnet_log_level level, const char* message, void* user) {
    App* app = (App*)user;
    if (app == NULL || app->closing) {
        return;
    }
    const char* tag = "info";
    if (level == MEWNET_LOG_WARNING) {
        tag = "warn";
    } else if (level == MEWNET_LOG_ERROR) {
        tag = "error";
    } else if (level == MEWNET_LOG_DEBUG) {
        tag = "debug";
    }
    log_line(app, "[mihomo/%s] %s", tag, message);
}

static bool open_services(App* app);

static void headless_signal(int sig) {
    if (sig == SIGUSR1) {
        g_headless_switch = 1;
    } else {
        g_headless_stop = 1;
    }
}

static int run_headless(App* app) {
    app->headless = true;
    mewnet_set_log_callback(on_mihomo_log, app);
    mewnet_set_log_level(MEWNET_LOG_WARNING);

    if (!open_services(app)) {
        cleanup(app);
        return 1;
    }

    signal(SIGINT, headless_signal);
    signal(SIGTERM, headless_signal);
    signal(SIGUSR1, headless_signal);

    log_line(app, "DB %s", app->options.db_path);
    log_line(app, "PROXY http://127.0.0.1:%d", app->actual_port);
    log_line(app, "batch=%zu concurrency=%zu fetch_interval=%ds slow_interval=%ds "
                  "pool_target=%d health_interval=%ds health_timeout=%dms",
             app->options.batch, app->options.concurrency,
             app->options.fetch_interval_sec, app->options.slow_interval_sec,
             app->options.pool_target, app->options.health_interval_sec,
             app->options.health_timeout_ms);

    worker_main(app);
    cleanup(app);
    return 0;
}

static bool open_services(App* app) {
    char* dir = g_path_get_dirname(app->options.db_path);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        g_free(dir);
        show_error(app, "Could not create database directory", app->options.db_path);
        return false;
    }
    g_free(dir);

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = app->options.db_path;
    config.test_url = app->options.test_url;
    config.test_expected = app->options.test_expected;
    config.test_timeout_ms = app->options.timeout_ms;
    config.request_limit = 10;
    config.request_window_seconds = 240;
    config.default_fetch_interval_sec = app->options.fetch_interval_sec;

    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        show_error(app, "Could not open database", opened.error.message);
        return false;
    }
    app->db = opened.db;

    mewnet_subs* state_db = app->db;
    if (!app->headless) {
        mewnet_subs_open_result ui_opened = mewnet_subs_open(&config);
        if (ui_opened.db == NULL) {
            show_error(app, "Could not open UI database", ui_opened.error.message);
            return false;
        }
        app->ui_db = ui_opened.db;
        state_db = app->ui_db;
    }

    mewnet_error err = mewnet_subs_default_group(state_db, &app->group_id);
    if (err.code != MEWNET_ERROR_NONE) {
        show_error(app, "Could not create subscription group", err.message);
        return false;
    }

    int priority = 1000;
    for (size_t i = 0; i < app->options.urls->len; i++) {
        const char* url = g_ptr_array_index(app->options.urls, i);
        err = mewnet_subs_add_subscription_in_group(state_db, app->group_id, url,
                                                    priority--, NULL);
        if (err.code != MEWNET_ERROR_NONE) {
            log_line(app, "ADD FAILED url=%s: %s", url, err.message);
        }
    }
    if (app->options.urls->len > 0) {
        mewnet_subscription_list list =
            mewnet_subs_load_subscriptions_in_group(state_db, app->group_id);
        if (list.error.code == MEWNET_ERROR_NONE && list.count > 0) {
            app->pending_fetch_id = list.items[list.count - 1].id;
            mewnet_subs_unload_subscriptions(list);
        }
    }

    mewnet_config milib_config = mewnet_config_default();
    milib_config.mixed_port = app->options.proxy_port;
    milib_config.log_level = "warning";

    mewnet_init_result init = mewnet_init(&milib_config);
    if (init.instance == NULL) {
        show_error(app, "Could not start local proxy", init.error.message);
        return false;
    }
    app->milib = init.instance;

    mewnet_status_info status = mewnet_get_status(app->milib);
    app->actual_port = status.mixed_port > 0 ? status.mixed_port : app->options.proxy_port;
    if (app->actual_port <= 0) {
        show_error(app, "Local proxy is not listening",
                   "Set --proxy-port or check the Mihomo configuration.");
        return false;
    }
    return true;
}

static gboolean take_screenshot(gpointer user) {
    App* app = (App*)user;
    int width = gtk_widget_get_width(app->window);
    int height = gtk_widget_get_height(app->window);
    if (width <= 0 || height <= 0) {
        return G_SOURCE_REMOVE;
    }
    GtkNative* native = gtk_widget_get_native(app->window);
    if (native == NULL) {
        return G_SOURCE_REMOVE;
    }
    GskRenderer* renderer = gtk_native_get_renderer(native);
    if (renderer == NULL) {
        return G_SOURCE_REMOVE;
    }
    GdkPaintable* paintable = gtk_widget_paintable_new(app->window);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(GDK_PAINTABLE(paintable), snapshot, width, height);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    graphene_rect_t rect = GRAPHENE_RECT_INIT(0, 0, width, height);
    GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &rect);
    if (texture != NULL) {
        if (gdk_texture_save_to_png(texture, app->options.screenshot_path)) {
            log_line(app, "SCREENSHOT %s", app->options.screenshot_path);
        }
        g_object_unref(texture);
    }
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    if (app->options.screenshot_exit) {
        GApplication* application = g_application_get_default();
        if (application != NULL) {
            g_application_quit(application);
        }
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_unix_signal(gpointer user) {
    (void)user;
    GApplication* application = g_application_get_default();
    if (application != NULL) {
        g_application_quit(application);
    }
    return G_SOURCE_REMOVE;
}

static void on_activate(GtkApplication* gtk_app, gpointer user) {
    (void)gtk_app;
    App* app = (App*)user;

    install_css();
    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "mewnet — proxy and discovery");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1280, 800);
    g_signal_connect(app->window, "close-request", G_CALLBACK(on_close_request), app);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->window), root);

    GtkWidget* header = gtk_header_bar_new();
    GtkWidget* title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>mewnet</b>  discovery control");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
    gtk_window_set_titlebar(GTK_WINDOW(app->window), header);

    gtk_box_append(GTK_BOX(root), build_status_panel(app));

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 440);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(root), paned);

    build_subscriptions_panel(app, paned);
    build_log_panel(app, paned);

    if (!open_services(app)) {
        gtk_widget_set_sensitive(root, FALSE);
        gtk_window_present(GTK_WINDOW(app->window));
        return;
    }

    app->actual_port = mewnet_get_status(app->milib).mixed_port;
    update_local_proxy_label(app);
    log_line(app, "DB %s", app->options.db_path);
    log_line(app, "PROXY http://127.0.0.1:%d", app->actual_port);
    log_line(app, "batch=%zu concurrency=%zu fetch_interval=%ds slow_interval=%ds",
             app->options.batch, app->options.concurrency,
             app->options.fetch_interval_sec, app->options.slow_interval_sec);

    reload_subscriptions(app);
    app->refresh_source = g_timeout_add(1000, refresh_tick, app);
    app->worker = g_thread_new("mewnet-discovery", worker_main, app);

    gtk_window_present(GTK_WINDOW(app->window));
    if (app->options.screenshot_path != NULL) {
        g_timeout_add(1500, take_screenshot, app);
    }
}

/* ------------------------------------------------------------------ */
/* command line                                                        */
/* ------------------------------------------------------------------ */

static void options_init(Options* options) {
    memset(options, 0, sizeof(*options));
    options->db_path = g_strdup("data/mewnet-gtk.db");
    options->proxy_port = 0;
    options->batch = 70;
    options->concurrency = 70;
    options->timeout_ms = 4000;
    options->fetch_interval_sec = 3600;
    options->slow_interval_sec = 24;
    options->pool_target = 4;
    options->health_interval_sec = 6;
    options->health_timeout_ms = 2500;
    options->test_url = g_strdup("https://www.gstatic.com/generate_204");
    options->test_expected = g_strdup("204");
    options->health_url = g_strdup("https://www.gstatic.com/generate_204");
    options->health_expected = g_strdup("204");
    options->run_seconds = 0;
    options->screenshot_path = NULL;
    options->screenshot_exit = false;
    options->urls = g_ptr_array_new_with_free_func(g_free);
}

static void options_free(Options* options) {
    g_free(options->db_path);
    g_free(options->test_url);
    g_free(options->test_expected);
    g_free(options->health_url);
    g_free(options->health_expected);
    g_free(options->screenshot_path);
    g_ptr_array_unref(options->urls);
}

static bool take_value(int argc, char** argv, int* index, const char** out, GError** error) {
    if (*index + 1 >= argc) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "option %s requires a value", argv[*index]);
        return false;
    }
    *out = argv[++(*index)];
    return true;
}

static bool parse_options(int argc, char** argv, Options* options, GError** error) {
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            g_print("usage: %s [options] [SUBSCRIPTION_URL...]\n"
                    "  --db PATH              sqlite database (default data/mewnet-gtk.db)\n"
                    "  --proxy-port N         mixed proxy port (0 = free port)\n"
                    "  --batch N              proxies per fast discovery batch (default 70)\n"
                    "  --concurrency N        concurrent checks (default 70)\n"
                    "  --timeout MS           per-proxy timeout (default 4000)\n"
                    "  --fetch-interval S     subscription refresh interval (default 3600)\n"
                    "  --slow-interval S      delay between checks when a proxy works (default 24)\n"
                    "  --pool-target N        verified proxies to keep ready (default 4)\n"
                    "  --health-interval S    active proxy health check interval (default 6)\n"
                    "  --health-timeout MS    active proxy health check timeout (default 2500)\n"
                    "  --health-url URL       health check URL (default gstatic 204)\n"
                    "  --health-expected CODE expected health status (default 204)\n"
                    "  --test-url URL         URL used for discovery tests\n"
                    "  --test-expected CODE   expected discovery status (default 204)\n"
                    "  --headless             run without GTK (for tests)\n"
                    "  --run-seconds N        stop headless run after N seconds\n"
                    "  --screenshot PATH      save a PNG of the window and exit\n",
                    argv[0]);
            exit(0);
        } else if (strcmp(arg, "--db") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            g_free(options->db_path);
            options->db_path = g_strdup(value);
        } else if (strcmp(arg, "--proxy-port") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            options->proxy_port = atoi(value);
        } else if (strcmp(arg, "--batch") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            int parsed = atoi(value);
            if (parsed > 0) options->batch = (size_t)parsed;
        } else if (strcmp(arg, "--concurrency") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            int parsed = atoi(value);
            if (parsed > 0) options->concurrency = (size_t)parsed;
        } else if (strcmp(arg, "--timeout") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            options->timeout_ms = atoi(value);
        } else if (strcmp(arg, "--fetch-interval") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            options->fetch_interval_sec = atoi(value);
        } else if (strcmp(arg, "--slow-interval") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            options->slow_interval_sec = atoi(value);
        } else if (strcmp(arg, "--pool-target") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            options->pool_target = atoi(value);
        } else if (strcmp(arg, "--health-interval") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            options->health_interval_sec = atoi(value);
        } else if (strcmp(arg, "--health-timeout") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            options->health_timeout_ms = atoi(value);
        } else if (strcmp(arg, "--health-url") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            g_free(options->health_url);
            options->health_url = g_strdup(value);
        } else if (strcmp(arg, "--health-expected") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            g_free(options->health_expected);
            options->health_expected = g_strdup(value);
        } else if (strcmp(arg, "--test-url") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            g_free(options->test_url);
            options->test_url = g_strdup(value);
        } else if (strcmp(arg, "--test-expected") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            g_free(options->test_expected);
            options->test_expected = g_strdup(value);
        } else if (strcmp(arg, "--headless") == 0) {
            options->headless = true;
        } else if (strcmp(arg, "--run-seconds") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            options->run_seconds = atoi(value);
        } else if (strcmp(arg, "--screenshot") == 0) {
            const char* value = NULL;
            if (!take_value(argc, argv, &i, &value, error)) return false;
            g_free(options->screenshot_path);
            options->screenshot_path = g_strdup(value);
            options->screenshot_exit = true;
        } else if (strncmp(arg, "--", 2) == 0) {
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_UNKNOWN_OPTION,
                        "unknown option %s", arg);
            return false;
        } else {
            g_ptr_array_add(options->urls, g_strdup(arg));
        }
    }
    if (options->batch == 0) options->batch = 70;
    if (options->concurrency == 0) options->concurrency = 70;
    if (options->timeout_ms <= 0) options->timeout_ms = 4000;
    if (options->fetch_interval_sec <= 0) options->fetch_interval_sec = 3600;
    if (options->slow_interval_sec <= 0) options->slow_interval_sec = 24;
    if (options->pool_target <= 0) options->pool_target = 4;
    if (options->health_interval_sec <= 0) options->health_interval_sec = 6;
    if (options->health_timeout_ms <= 0) options->health_timeout_ms = 2500;
    return true;
}

int main(int argc, char** argv) {
    App app;
    memset(&app, 0, sizeof(app));
    g_mutex_init(&app.lock);
    g_cond_init(&app.cond);
    app.sub_ids = g_array_new(FALSE, FALSE, sizeof(int64_t));

    GError* error = NULL;
    options_init(&app.options);
    if (!parse_options(argc, argv, &app.options, &error)) {
        fprintf(stderr, "%s\n", error != NULL ? error->message : "invalid arguments");
        g_clear_error(&error);
        options_free(&app.options);
        return 2;
    }

    if (app.options.headless) {
        int rc = run_headless(&app);
        g_array_unref(app.sub_ids);
        options_free(&app.options);
        g_cond_clear(&app.cond);
        g_mutex_clear(&app.lock);
        return rc;
    }

    mewnet_set_log_callback(on_mihomo_log, &app);
    mewnet_set_log_level(MEWNET_LOG_WARNING);

    GtkApplication* gtk_app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), &app);
    g_unix_signal_add(SIGINT, on_unix_signal, &app);
    g_unix_signal_add(SIGTERM, on_unix_signal, &app);

    char* gtk_argv[] = {argv[0], NULL};
    int status = g_application_run(G_APPLICATION(gtk_app), 1, gtk_argv);

    cleanup(&app);
    g_object_unref(gtk_app);
    g_array_unref(app.sub_ids);
    options_free(&app.options);
    g_cond_clear(&app.cond);
    g_mutex_clear(&app.lock);
    return status;
}
