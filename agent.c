#include "mewnet_subs_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

struct mewnet_agent {
    mewnet_agent_config cfg;
    mewnet_subs* fetch_db;
    mewnet_subs* discovery_db;
    char health_url[MEWNET_URL_MAX];
    char health_expected[64];
    mewnet_agent_callbacks callbacks;

    mewnet_mutex lock;
    bool stop;
    bool switch_requested;
    bool emergency_fetch;
    bool pool_batch_requested;
    int64_t pending_fetch_id;

    int64_t current_id;
    int64_t last_switch_id;
    char current_name[MEWNET_NAME_MAX];
    char current_protocol[MEWNET_TYPE_MAX];
    char current_server[MEWNET_HOST_MAX];
    int current_port;
    unsigned current_delay;
    bool ready;

    unsigned health_history;
    unsigned health_ewma;
    int slow_streak;
    int64_t last_slow_switch_ms;
    int64_t next_health_ms;
    int64_t next_discover_ms;
    int64_t last_pool_batch_ms;
    int64_t last_state_ms;
    int64_t last_counts_ms;
    int64_t cached_subscriptions;
    int64_t cached_proxies;
    int64_t cached_alive;
    int64_t cached_untested;

#ifdef _WIN32
    HANDLE thread;
    HANDLE fetch_thread;
    HANDLE discovery_thread;
#else
    pthread_t thread;
    pthread_t fetch_thread;
    pthread_t discovery_thread;
#endif
    bool thread_started;
    bool fetch_thread_started;
    bool discovery_thread_started;
};

mewnet_agent_config mewnet_agent_config_default(void) {
    mewnet_agent_config config;
    memset(&config, 0, sizeof(config));
    config.batch = 70;
    config.concurrency = 70;
    config.fetch_interval_sec = 3600;
    config.slow_interval_sec = 24;
    config.pool_target = 12;
    config.health_interval_sec = 6;
    config.health_timeout_ms = 2500;
    config.health_url = "https://lua.org";
    config.health_expected = "200";
    config.slow_factor = 3;
    config.slow_floor_ms = 1000;
    config.slow_checks = 3;
    config.slow_cooldown_sec = 600;
    return config;
}

static void agent_log(mewnet_agent* agent, const char* format, ...) {
    if (agent->callbacks.log == NULL) {
        return;
    }
    char text[1024];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    agent->callbacks.log(text, agent->callbacks.user);
}

static void agent_sleep(mewnet_agent* agent, int ms) {
    int remaining = ms;
    while (remaining > 0) {
        int chunk = remaining > 100 ? 100 : remaining;
        subs_sleep_ms(chunk);
        remaining -= chunk;
        subs_mutex_lock(&agent->lock);
        bool stop = agent->stop;
        bool pending = agent->pending_fetch_id != 0;
        bool switching = agent->switch_requested;
        subs_mutex_unlock(&agent->lock);
        if (stop || pending || switching) {
            return;
        }
    }
}

static bool agent_stopped(mewnet_agent* agent) {
    subs_mutex_lock(&agent->lock);
    bool stop = agent->stop;
    subs_mutex_unlock(&agent->lock);
    return stop;
}

static int64_t agent_take_pending(mewnet_agent* agent) {
    subs_mutex_lock(&agent->lock);
    int64_t id = agent->pending_fetch_id;
    agent->pending_fetch_id = 0;
    subs_mutex_unlock(&agent->lock);
    return id;
}

static bool agent_take_switch(mewnet_agent* agent) {
    subs_mutex_lock(&agent->lock);
    bool value = agent->switch_requested;
    agent->switch_requested = false;
    subs_mutex_unlock(&agent->lock);
    return value;
}

static void agent_copy_current(mewnet_agent* agent, int64_t* id, char* name, size_t name_size,
                               char* protocol, size_t protocol_size,
                               char* server, size_t server_size, int* port,
                               unsigned* delay, bool* ready) {
    subs_mutex_lock(&agent->lock);
    if (id != NULL) *id = agent->current_id;
    if (name != NULL) snprintf(name, name_size, "%s", agent->current_name);
    if (protocol != NULL) snprintf(protocol, protocol_size, "%s", agent->current_protocol);
    if (server != NULL) snprintf(server, server_size, "%s", agent->current_server);
    if (port != NULL) *port = agent->current_port;
    if (delay != NULL) *delay = agent->current_delay;
    if (ready != NULL) *ready = agent->ready;
    subs_mutex_unlock(&agent->lock);
}

static void agent_clear_current(mewnet_agent* agent) {
    subs_mutex_lock(&agent->lock);
    agent->current_id = 0;
    agent->current_name[0] = '\0';
    subs_mutex_unlock(&agent->lock);
}

static void agent_invalidate_counts(mewnet_agent* agent) {
    subs_mutex_lock(&agent->lock);
    agent->last_counts_ms = 0;
    subs_mutex_unlock(&agent->lock);
}

static void agent_refresh_counts(mewnet_agent* agent, bool force) {
    int64_t now = subs_now_ms();
    subs_mutex_lock(&agent->lock);
    int64_t last = agent->last_counts_ms;
    subs_mutex_unlock(&agent->lock);
    if (!force && last != 0 && now - last < 2000) {
        return;
    }
    int64_t subscriptions = 0, proxies = 0, alive = 0, untested = 0;
    mewnet_subs_count_subscriptions(agent->cfg.db, agent->cfg.group_id, &subscriptions);
    mewnet_subs_count_proxies(agent->cfg.db, agent->cfg.group_id, &proxies);
    mewnet_subs_count_alive(agent->cfg.db, agent->cfg.group_id, &alive);
    mewnet_subs_count_untested(agent->cfg.db, agent->cfg.group_id, &untested);
    subs_mutex_lock(&agent->lock);
    agent->cached_subscriptions = subscriptions;
    agent->cached_proxies = proxies;
    agent->cached_alive = alive;
    agent->cached_untested = untested;
    agent->last_counts_ms = now;
    subs_mutex_unlock(&agent->lock);
}

static void agent_counts(mewnet_agent* agent, int64_t* subscriptions, int64_t* proxies,
                         int64_t* alive, int64_t* untested) {
    agent_refresh_counts(agent, false);
    subs_mutex_lock(&agent->lock);
    if (subscriptions != NULL) *subscriptions = agent->cached_subscriptions;
    if (proxies != NULL) *proxies = agent->cached_proxies;
    if (alive != NULL) *alive = agent->cached_alive;
    if (untested != NULL) *untested = agent->cached_untested;
    subs_mutex_unlock(&agent->lock);
}

static void agent_emit_state(mewnet_agent* agent) {
    mewnet_agent_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    int64_t subscriptions = 0, proxies = 0, alive = 0, untested = 0;
    agent_counts(agent, &subscriptions, &proxies, &alive, &untested);

    subs_mutex_lock(&agent->lock);
    snapshot.proxy_id = agent->current_id;
    snprintf(snapshot.name, sizeof(snapshot.name), "%s", agent->current_name);
    snprintf(snapshot.protocol, sizeof(snapshot.protocol), "%s", agent->current_protocol);
    snprintf(snapshot.server, sizeof(snapshot.server), "%s", agent->current_server);
    snapshot.port = agent->current_port;
    snapshot.delay_ms = agent->current_delay;
    snapshot.ready = agent->ready;
    subs_mutex_unlock(&agent->lock);

    snapshot.subscriptions = subscriptions;
    snapshot.proxies = proxies;
    snapshot.alive = alive;
    snapshot.untested = untested;
    agent->last_state_ms = subs_now_ms();
    if (agent->callbacks.state != NULL) {
        agent->callbacks.state(&snapshot, agent->callbacks.user);
    }
}

static bool agent_health_check(mewnet_agent* agent, mewnet_probe_result* out) {
    char name[MEWNET_NAME_MAX];
    agent_copy_current(agent, NULL, name, sizeof(name), NULL, 0, NULL, 0, NULL, NULL, NULL);
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (name[0] == '\0') {
        return false;
    }
    int expected = atoi(agent->health_expected);
    mewnet_probe_result result = mewnet_probe(agent->cfg.milib, agent->health_url,
                                              expected, agent->cfg.health_timeout_ms);
    if (out != NULL) {
        *out = result;
    }
    return result.ok;
}

static void agent_mark_current_dead(mewnet_agent* agent, const char* reason) {
    (void)reason;
    int64_t id;
    char name[MEWNET_NAME_MAX];
    agent_copy_current(agent, &id, name, sizeof(name), NULL, 0, NULL, 0, NULL, NULL, NULL);
    if (id == 0) {
        return;
    }
    agent_log(agent, "HEALTH FAIL id=%lld name=%s (kept alive)",
              (long long)id, name[0] != '\0' ? name : "(unknown)");
    agent_clear_current(agent);
    agent->health_ewma = 0;
    agent->slow_streak = 0;
}

static bool agent_route(mewnet_agent* agent, const mewnet_subs_proxy* proxy) {
    mewnet_status_info before = mewnet_get_status(agent->cfg.milib);
    char previous[MEWNET_NAME_MAX];
    snprintf(previous, sizeof(previous), "%s", before.selected);

    mewnet_add_options options = mewnet_add_options_default();
    options.select_proxy = true;
    mewnet_error_code code = mewnet_add_proxy(agent->cfg.milib, proxy->link, &options);
    if (code != MEWNET_ERROR_NONE) {
        agent_log(agent, "ROUTE FAILED id=%lld: code %d", (long long)proxy->id, (int)code);
        return false;
    }

    mewnet_status_info status = mewnet_get_status(agent->cfg.milib);
    if (previous[0] != '\0' && strcmp(previous, status.selected) != 0 &&
        strcmp(previous, "DIRECT") != 0) {
        mewnet_remove_proxy(agent->cfg.milib, previous);
    }

    char display_name[MEWNET_NAME_MAX];
    snprintf(display_name, sizeof(display_name), "%s",
             status.selected[0] != '\0' ? status.selected : proxy->name);

    bool first = false;
    subs_mutex_lock(&agent->lock);
    first = !agent->ready;
    agent->current_id = proxy->id;
    snprintf(agent->current_name, sizeof(agent->current_name), "%s", display_name);
    snprintf(agent->current_protocol, sizeof(agent->current_protocol), "%s", proxy->protocol);
    snprintf(agent->current_server, sizeof(agent->current_server), "%s", proxy->server);
    agent->current_port = proxy->port;
    agent->current_delay = proxy->last_delay_ms;
    agent->ready = true;
    subs_mutex_unlock(&agent->lock);

    agent_log(agent, "ACTIVE id=%lld name=%s delay=%u",
              (long long)proxy->id,
              display_name[0] != '\0' ? display_name : "(manual)",
              proxy->last_delay_ms);
    if (first) {
        agent_log(agent, "READY local=http://127.0.0.1:%d", agent->cfg.proxy_port);
    }
    agent->health_history = 0;
    agent->health_ewma = 0;
    agent->slow_streak = 0;
    agent_emit_state(agent);
    return true;
}

static mewnet_subs_proxy agent_get_proxy(mewnet_agent* agent, int64_t id) {
    mewnet_subs_proxy proxy;
    memset(&proxy, 0, sizeof(proxy));
    mewnet_subs_get_proxy(agent->cfg.db, id, &proxy);
    return proxy;
}

static bool agent_run_batch_db(mewnet_agent* agent, mewnet_subs* db,
                               const char* reason, bool route_found);

static bool agent_run_batch(mewnet_agent* agent, const char* reason, bool route_found) {
    return agent_run_batch_db(agent, agent->cfg.db, reason, route_found);
}

static bool agent_run_batch_db(mewnet_agent* agent, mewnet_subs* db,
                               const char* reason, bool route_found) {
    mewnet_subs_batch_test_result tested =
        mewnet_subs_test_batch(db, agent->cfg.milib, agent->cfg.group_id,
                               agent->cfg.batch, agent->cfg.concurrency);
    if (tested.error.code != MEWNET_ERROR_NONE) {
        agent_log(agent, "BATCH FAILED reason=%s: %s", reason, tested.error.message);
        return false;
    }
    agent_log(agent, "BATCH reason=%s tested=%zu alive=%zu", reason,
              tested.tested, tested.alive);
    agent_invalidate_counts(agent);
    if (tested.alive > 0) {
        if (!route_found) {
            agent_log(agent, "DISCOVERED id=%lld", (long long)tested.first_alive_proxy_id);
            return true;
        }
        /* Activate the best measured node, not whichever finished first:
         * first_alive ranks by health_failures, rating, TTFB and speed. */
        mewnet_subs_proxy best;
        if (mewnet_subs_first_alive(db, agent->cfg.group_id, &best).code == MEWNET_ERROR_NONE) {
            return agent_route(agent, &best);
        }
    }
    return false;
}

static bool agent_route_first_alive(mewnet_agent* agent) {
    mewnet_subs_proxy proxy;
    mewnet_error err = mewnet_subs_first_alive(agent->cfg.db, agent->cfg.group_id, &proxy);
    if (err.code != MEWNET_ERROR_NONE) {
        return false;
    }
    return agent_route(agent, &proxy);
}

static bool agent_switch_verified(mewnet_agent* agent) {
    int64_t current_id;
    agent_copy_current(agent, &current_id, NULL, 0, NULL, 0, NULL, 0, NULL, NULL, NULL);

    int64_t alive = 0;
    mewnet_subs_count_alive(agent->cfg.db, agent->cfg.group_id, &alive);
    if (alive == 0) {
        return false;
    }

    int64_t exclude = current_id;
    for (int64_t attempt = 0; attempt < alive; attempt++) {
        mewnet_subs_proxy proxy;
        mewnet_error err = mewnet_subs_random_best_alive(
            agent->cfg.db, agent->cfg.group_id, exclude, 0, 5, &proxy);
        if (err.code == MEWNET_ERROR_NOT_FOUND) {
            break;
        }
        if (err.code != MEWNET_ERROR_NONE) {
            agent_log(agent, "SWITCH FAILED: %s", err.message);
            break;
        }
        exclude = proxy.id;
        if (!agent_route(agent, &proxy)) {
            mewnet_subs_record_test(agent->cfg.db, proxy.id, false, 0, "route failed");
            continue;
        }
        if (agent_health_check(agent, NULL)) {
            if (current_id != 0) {
                agent_log(agent, "SWITCH id=%lld name=%s", (long long)proxy.id,
                          proxy.name[0] != '\0' ? proxy.name : "(manual)");
            }
            return true;
        }
        agent_log(agent, "SWITCH candidate id=%lld failed verification", (long long)proxy.id);
        agent_mark_current_dead(agent, "failover verification failed");
        exclude = proxy.id;
    }
    agent_log(agent, "SWITCH failed: no verified working proxy");
    return false;
}

static bool agent_switch_random(mewnet_agent* agent) {
    int64_t current_id;
    agent_copy_current(agent, &current_id, NULL, 0, NULL, 0, NULL, 0, NULL, NULL, NULL);

    int64_t alive = 0;
    mewnet_subs_count_alive(agent->cfg.db, agent->cfg.group_id, &alive);
    if (alive <= 1) {
        return false;
    }

    int64_t exclude = current_id;
    int64_t avoid = agent->last_switch_id;
    for (int64_t attempt = 0; attempt < alive; attempt++) {
        mewnet_subs_proxy proxy;
        mewnet_error err = mewnet_subs_random_best_alive(
            agent->cfg.db, agent->cfg.group_id, exclude, avoid, 5, &proxy);
        if (err.code == MEWNET_ERROR_NOT_FOUND && avoid != 0) {
            avoid = 0;
            err = mewnet_subs_random_best_alive(
                agent->cfg.db, agent->cfg.group_id, exclude, avoid, 5, &proxy);
        }
        if (err.code == MEWNET_ERROR_NOT_FOUND) {
            break;
        }
        if (err.code != MEWNET_ERROR_NONE) {
            agent_log(agent, "SWITCH FAILED: %s", err.message);
            break;
        }
        if (!agent_route(agent, &proxy)) {
            mewnet_subs_record_test(agent->cfg.db, proxy.id, false, 0, "route failed");
            exclude = proxy.id;
            continue;
        }
        if (agent_health_check(agent, NULL)) {
            agent->last_switch_id = current_id;
            agent_log(agent, "SWITCH random id=%lld name=%s", (long long)proxy.id,
                      proxy.name[0] != '\0' ? proxy.name : "(manual)");
            return true;
        }
        agent_log(agent, "SWITCH candidate id=%lld failed verification", (long long)proxy.id);
        agent_mark_current_dead(agent, "switch verification failed");
        exclude = 0;
        avoid = 0;
    }
    agent_log(agent, "SWITCH failed: no verified working proxy");
    return false;
}

/* The active proxy answers health checks but is far slower than the pool:
 * a node that "works" but is 3x slower than its neighbours is a bad active
 * choice even though it never fails. The EWMA keeps a single spike from
 * triggering, the median gives a baseline, and the cooldown keeps a globally
 * slow network from rotating the whole pool. */
static bool agent_slow_confirmed(mewnet_agent* agent, unsigned ewma_ms) {
    if (agent->cfg.slow_factor <= 0 || agent->cfg.slow_floor_ms <= 0 ||
        agent->cfg.slow_checks <= 0) {
        return false;
    }
    if (ewma_ms < (unsigned)agent->cfg.slow_floor_ms) {
        agent->slow_streak = 0;
        return false;
    }
    int64_t current_id = 0;
    agent_copy_current(agent, &current_id, NULL, 0, NULL, 0, NULL, 0, NULL, NULL, NULL);
    if (current_id == 0) {
        return false;
    }
    unsigned median_ms = 0;
    size_t samples = 0;
    mewnet_error err = mewnet_subs_median_delay(agent->cfg.db, agent->cfg.group_id,
                                                current_id, 5, &median_ms, &samples);
    if (err.code != MEWNET_ERROR_NONE || samples < 2 || median_ms == 0) {
        agent->slow_streak = 0;
        return false;
    }
    if ((uint64_t)ewma_ms <= (uint64_t)median_ms * (uint64_t)agent->cfg.slow_factor) {
        agent->slow_streak = 0;
        return false;
    }
    if (agent->last_slow_switch_ms != 0 &&
        subs_now_ms() - agent->last_slow_switch_ms <
            (int64_t)agent->cfg.slow_cooldown_sec * 1000) {
        return false;
    }
    agent->slow_streak++;
    agent_log(agent, "SLOW try=%d/%d ewma=%ums pool_median=%ums samples=%zu",
              agent->slow_streak, agent->cfg.slow_checks, ewma_ms, median_ms, samples);
    if (agent->slow_streak < agent->cfg.slow_checks) {
        return false;
    }
    agent->slow_streak = 0;
    return true;
}

static void* agent_fetch_thread(void* data) {
    mewnet_agent* agent = (mewnet_agent*)data;
    int64_t last_fetch_ms = 0;
    int64_t last_new_ms = 0;
    int64_t last_emergency_ms = 0;
    mewnet_subs_max_last_fetch(agent->fetch_db, agent->cfg.group_id, &last_fetch_ms);

    while (!agent_stopped(agent)) {
        int64_t id = agent_take_pending(agent);
        const char* tag = " (new)";
        int64_t now = subs_now_ms();

        subs_mutex_lock(&agent->lock);
        bool emergency = agent->emergency_fetch;
        agent->emergency_fetch = false;
        subs_mutex_unlock(&agent->lock);

        if (id == 0 && emergency && now - last_emergency_ms >= 5000) {
            last_emergency_ms = now;
            mewnet_subs_next_subscription_id(agent->fetch_db, agent->cfg.group_id,
                                             false, &id);
            tag = " (emergency)";
        }
        if (id == 0 && now - last_new_ms >= 5000) {
            last_new_ms = now;
            mewnet_subs_next_subscription_id(agent->fetch_db, agent->cfg.group_id,
                                             true, &id);
            tag = " (new)";
        }
        if (id == 0) {
            int64_t interval = (int64_t)agent->cfg.fetch_interval_sec * 1000;
            if (last_fetch_ms == 0 || now - last_fetch_ms >= interval) {
                mewnet_subs_next_subscription_id(agent->fetch_db, agent->cfg.group_id,
                                                 false, &id);
                tag = "";
            }
        }
        if (id == 0) {
            agent_sleep(agent, 500);
            continue;
        }

        mewnet_subs_fetch_result fetched = mewnet_subs_fetch_now(agent->fetch_db, id);
        if (fetched.error.code == MEWNET_ERROR_NONE) {
            last_fetch_ms = subs_now_ms();
            agent_invalidate_counts(agent);
            agent_log(agent, "FETCH id=%lld status=%d added=%zu updated=%zu removed=%zu%s",
                      (long long)fetched.subscription_id, fetched.http_status,
                      fetched.added, fetched.updated, fetched.removed, tag);
        } else if (fetched.error.code != MEWNET_ERROR_CANCELLED) {
            agent_log(agent, "FETCH FAILED id=%lld%s: %s", (long long)id, tag,
                      fetched.error.message);
            agent_sleep(agent, 1000);
        }
    }
    return NULL;
}

static void* agent_discovery_thread(void* data) {
    mewnet_agent* agent = (mewnet_agent*)data;
    int64_t next_discover = subs_now_ms();
    while (!agent_stopped(agent)) {
        subs_mutex_lock(&agent->lock);
        bool pool_batch = agent->pool_batch_requested;
        agent->pool_batch_requested = false;
        subs_mutex_unlock(&agent->lock);

        int64_t alive = 0;
        mewnet_subs_count_alive(agent->discovery_db, agent->cfg.group_id, &alive);
        if (alive == 0) {
            agent_sleep(agent, 500);
            continue;
        }
        if (pool_batch) {
            agent_run_batch_db(agent, agent->discovery_db, "pool", false);
            next_discover = subs_now_ms() + 1500;
            continue;
        }
        int64_t now = subs_now_ms();
        if (now < next_discover) {
            agent_sleep(agent, 200);
            continue;
        }
        int64_t interval = alive < agent->cfg.pool_target
                               ? 2000
                               : (int64_t)agent->cfg.slow_interval_sec * 1000;
        next_discover = now + interval;

        mewnet_subs_test_result result = mewnet_subs_test_candidate_now(
            agent->discovery_db, agent->cfg.milib, MEWNET_SUBS_CANDIDATE_UNTESTED);
        if (result.error.code == MEWNET_ERROR_NOT_FOUND) {
            result = mewnet_subs_test_candidate_now(
                agent->discovery_db, agent->cfg.milib, MEWNET_SUBS_CANDIDATE_NOT_ALIVE);
        }
        agent_invalidate_counts(agent);
        if (result.error.code == MEWNET_ERROR_NONE) {
            agent_log(agent, "TEST id=%lld alive=%d delay=%u rating=%d->%d",
                      (long long)result.proxy_id, result.alive ? 1 : 0,
                      result.delay_ms, result.rating_before, result.rating_after);
            if (result.alive) {
                agent_log(agent, "DISCOVERED id=%lld delay=%u",
                          (long long)result.proxy_id, result.delay_ms);
            }
        } else if (result.error.code != MEWNET_ERROR_NOT_FOUND) {
            agent_log(agent, "TEST FAILED: %s", result.error.message);
        }
    }
    return NULL;
}

static void* agent_thread_main(void* data) {
    mewnet_agent* agent = (mewnet_agent*)data;
    int64_t started = subs_now_ms();
    int64_t last_subs = -1;
    bool waiting_logged = false;
    agent->next_health_ms = started + 1500;
    agent->next_discover_ms = started;

    agent_refresh_counts(agent, true);
    agent_log(agent, "DISCOVERY start batch=%zu concurrency=%zu pool_target=%d",
              agent->cfg.batch, agent->cfg.concurrency, agent->cfg.pool_target);
    agent_emit_state(agent);

    while (!agent_stopped(agent)) {
        if (agent->cfg.run_seconds > 0 &&
            subs_now_ms() - started >= (int64_t)agent->cfg.run_seconds * 1000) {
            agent_log(agent, "RUN complete seconds=%d", agent->cfg.run_seconds);
            break;
        }

        int64_t subs = 0, proxies = 0, alive = 0, untested = 0;
        agent_counts(agent, &subs, &proxies, &alive, &untested);
        if (subs != last_subs) {
            last_subs = subs;
            agent_log(agent, "SUBS n=%lld", (long long)subs);
        }
        if (subs == 0) {
            if (!waiting_logged) {
                agent_log(agent, "WAIT no subscriptions yet - add a URL");
                waiting_logged = true;
            }
            agent_sleep(agent, 500);
            continue;
        }
        waiting_logged = false;

        int64_t now = subs_now_ms();

        if (alive == 0) {
            if (proxies == 0) {
                subs_mutex_lock(&agent->lock);
                agent->emergency_fetch = true;
                subs_mutex_unlock(&agent->lock);
                agent_sleep(agent, 500);
                continue;
            }
            if (agent_run_batch(agent, "fast", true)) {
                agent->next_health_ms = subs_now_ms() + 1500;
                agent->next_discover_ms = subs_now_ms() + 500;
                continue;
            }
            subs_mutex_lock(&agent->lock);
            agent->emergency_fetch = true;
            subs_mutex_unlock(&agent->lock);
            agent_sleep(agent, 1000);
            continue;
        }

        if (agent_take_switch(agent)) {
            agent_log(agent, "SWITCH requested");
            if (!agent_switch_random(agent)) {
                agent_log(agent, "SWITCH no other working proxy; looking for candidates");
                if (agent_run_batch(agent, "switch", true)) {
                    agent->ready = true;
                } else {
                    subs_mutex_lock(&agent->lock);
                    agent->emergency_fetch = true;
                    subs_mutex_unlock(&agent->lock);
                }
            }
            if (alive < agent->cfg.pool_target) {
                subs_mutex_lock(&agent->lock);
                agent->pool_batch_requested = true;
                subs_mutex_unlock(&agent->lock);
            }
            agent->next_health_ms = subs_now_ms() + 1500;
            agent->next_discover_ms = subs_now_ms() + 500;
            continue;
        }

        int64_t current_id;
        agent_copy_current(agent, &current_id, NULL, 0, NULL, 0, NULL, 0, NULL, NULL, NULL);
        if (current_id == 0) {
            if (agent_route_first_alive(agent)) {
                agent->next_health_ms = subs_now_ms() + 1500;
            } else {
                agent_sleep(agent, 500);
            }
            continue;
        }

        mewnet_subs_proxy current = agent_get_proxy(agent, current_id);
        if (current.id == 0) {
            agent_clear_current(agent);
            continue;
        }
        if (!current.alive) {
            agent_log(agent, "ACTIVE id=%lld is dead; failing over", (long long)current_id);
            if (!agent_switch_verified(agent)) {
                agent_clear_current(agent);
            }
            agent->next_health_ms = subs_now_ms() + 1500;
            continue;
        }

        if (now >= agent->next_health_ms) {
            mewnet_probe_result probe;
            memset(&probe, 0, sizeof(probe));
            bool healthy = agent_health_check(agent, &probe);
            unsigned delay_ms = (unsigned)probe.delay_ms;
            agent->health_history = ((agent->health_history << 1) | (healthy ? 0u : 1u)) & 0x07u;
            unsigned recent_failures = 0;
            for (unsigned bits = agent->health_history; bits != 0; bits >>= 1) {
                recent_failures += bits & 1u;
            }
            if (healthy) {
                int64_t current = 0;
                agent_copy_current(agent, &current, NULL, 0, NULL, 0, NULL, 0, NULL, NULL, NULL);
                if (current != 0) {
                    mewnet_subs_record_health_metrics(
                        agent->cfg.db, current, true, delay_ms, probe.ttfb_ms,
                        probe.ttlb_ms, probe.bytes, probe.speed_bps);
                    if (delay_ms > 0) {
                        agent_log(agent, "HEALTH id=%lld ttfb=%ums ttlb=%ums speed=%lluB/s",
                                  (long long)current, probe.ttfb_ms, probe.ttlb_ms,
                                  probe.speed_bps);
                    }
                }
                if (delay_ms > 0) {
                    /* EWMA with a 1/4 weight: a single spike must not count. */
                    agent->health_ewma = agent->health_ewma == 0
                                             ? delay_ms
                                             : (agent->health_ewma * 3 + delay_ms) / 4;
                    subs_mutex_lock(&agent->lock);
                    agent->current_delay = delay_ms;
                    subs_mutex_unlock(&agent->lock);
                    if (agent_slow_confirmed(agent, agent->health_ewma)) {
                        agent_log(agent, "SLOW switching id=%lld ewma=%ums",
                                  (long long)current, agent->health_ewma);
                        agent->last_slow_switch_ms = subs_now_ms();
                        if (!agent_switch_verified(agent)) {
                            agent_clear_current(agent);
                        }
                        agent->next_health_ms = subs_now_ms() + 1500;
                        agent_emit_state(agent);
                        continue;
                    }
                }
                agent->next_health_ms = now + (int64_t)agent->cfg.health_interval_sec * 1000;
            } else {
                int64_t current = 0;
                agent_copy_current(agent, &current, NULL, 0, NULL, 0, NULL, 0, NULL, NULL, NULL);
                if (current != 0) {
                    mewnet_subs_record_health_metrics(agent->cfg.db, current, false, 0, 0, 0, 0, 0);
                }
                agent->slow_streak = 0;
                agent_log(agent, "HEALTH FAIL recent=%u/3", recent_failures);
                if (recent_failures >= 2) {
                    agent_mark_current_dead(agent, "health check failed");
                    agent->health_history = 0;
                    if (!agent_switch_verified(agent)) {
                        agent_clear_current(agent);
                    }
                    agent->next_health_ms = now + 1500;
                } else {
                    agent->next_health_ms = now + 1000;
                }
            }
            agent_emit_state(agent);
            continue;
        }

        if (subs_now_ms() - agent->last_state_ms >= 1000) {
            agent_emit_state(agent);
        }
        agent_sleep(agent, 200);
    }

    agent_log(agent, "DISCOVERY stopped");
    agent_emit_state(agent);
    return NULL;
}

#ifdef _WIN32
static DWORD WINAPI agent_thread_win(LPVOID data) {
    agent_thread_main(data);
    return 0;
}

static DWORD WINAPI agent_discovery_thread_win(LPVOID data) {
    agent_discovery_thread(data);
    return 0;
}

static DWORD WINAPI agent_fetch_thread_win(LPVOID data) {
    agent_fetch_thread(data);
    return 0;
}
#endif

mewnet_agent* mewnet_agent_start(const mewnet_agent_config* config,
                                 mewnet_agent_callbacks callbacks) {
    if (config == NULL || config->db == NULL || config->milib == NULL) {
        return NULL;
    }
    mewnet_agent* agent = (mewnet_agent*)calloc(1, sizeof(mewnet_agent));
    if (agent == NULL) {
        return NULL;
    }
    agent->cfg = *config;
    agent->cfg.batch = config->batch > 0 ? config->batch : 70;
    agent->cfg.concurrency = config->concurrency > 0 ? config->concurrency : 70;
    agent->cfg.pool_target = config->pool_target > 0 ? config->pool_target : 12;
    agent->cfg.health_interval_sec = config->health_interval_sec > 0
                                         ? config->health_interval_sec : 6;
    agent->cfg.health_timeout_ms = config->health_timeout_ms > 0
                                       ? config->health_timeout_ms : 2500;
    agent->cfg.fetch_interval_sec = config->fetch_interval_sec > 0
                                        ? config->fetch_interval_sec : 3600;
    agent->cfg.slow_interval_sec = config->slow_interval_sec > 0
                                       ? config->slow_interval_sec : 24;
    agent->cfg.slow_factor = config->slow_factor > 0 ? config->slow_factor : 0;
    agent->cfg.slow_floor_ms = config->slow_floor_ms > 0 ? config->slow_floor_ms : 1000;
    agent->cfg.slow_checks = config->slow_checks > 0 ? config->slow_checks : 3;
    agent->cfg.slow_cooldown_sec = config->slow_cooldown_sec > 0
                                       ? config->slow_cooldown_sec : 600;
    snprintf(agent->health_url, sizeof(agent->health_url), "%s",
             config->health_url != NULL ? config->health_url
                                        : "https://www.gstatic.com/generate_204");
    snprintf(agent->health_expected, sizeof(agent->health_expected), "%s",
             config->health_expected != NULL ? config->health_expected : "204");
    agent->callbacks = callbacks;
    agent->fetch_db = mewnet_subs_clone(agent->cfg.db);
    agent->discovery_db = mewnet_subs_clone(agent->cfg.db);
    if (agent->fetch_db == NULL || agent->discovery_db == NULL) {
        if (agent->fetch_db) mewnet_subs_close(agent->fetch_db);
        if (agent->discovery_db) mewnet_subs_close(agent->discovery_db);
        free(agent);
        return NULL;
    }
    subs_mutex_init(&agent->lock);

#ifdef _WIN32
    agent->fetch_thread = CreateThread(NULL, 0, agent_fetch_thread_win, agent, 0, NULL);
    if (agent->fetch_thread == NULL) {
        mewnet_subs_close(agent->fetch_db);
        mewnet_subs_close(agent->discovery_db);
        subs_mutex_destroy(&agent->lock);
        free(agent);
        return NULL;
    }
    agent->discovery_thread = CreateThread(NULL, 0, agent_discovery_thread_win, agent, 0, NULL);
    agent->thread = CreateThread(NULL, 0, agent_thread_win, agent, 0, NULL);
    if (agent->thread == NULL || agent->discovery_thread == NULL) {
        subs_mutex_lock(&agent->lock);
        agent->stop = true;
        subs_mutex_unlock(&agent->lock);
        if (agent->fetch_thread) {
            WaitForSingleObject(agent->fetch_thread, INFINITE);
            CloseHandle(agent->fetch_thread);
        }
        if (agent->discovery_thread) {
            WaitForSingleObject(agent->discovery_thread, INFINITE);
            CloseHandle(agent->discovery_thread);
        }
        mewnet_subs_close(agent->fetch_db);
        mewnet_subs_close(agent->discovery_db);
        subs_mutex_destroy(&agent->lock);
        free(agent);
        return NULL;
    }
#else
    if (pthread_create(&agent->fetch_thread, NULL, agent_fetch_thread, agent) != 0) {
        mewnet_subs_close(agent->fetch_db);
        subs_mutex_destroy(&agent->lock);
        free(agent);
        return NULL;
    }
    if (pthread_create(&agent->discovery_thread, NULL, agent_discovery_thread, agent) != 0 ||
        pthread_create(&agent->thread, NULL, agent_thread_main, agent) != 0) {
        subs_mutex_lock(&agent->lock);
        agent->stop = true;
        subs_mutex_unlock(&agent->lock);
        pthread_join(agent->fetch_thread, NULL);
        if (agent->discovery_thread != 0) pthread_join(agent->discovery_thread, NULL);
        mewnet_subs_close(agent->fetch_db);
        mewnet_subs_close(agent->discovery_db);
        subs_mutex_destroy(&agent->lock);
        free(agent);
        return NULL;
    }
#endif
    agent->thread_started = true;
    agent->fetch_thread_started = true;
    agent->discovery_thread_started = true;
    return agent;
}

void mewnet_agent_stop(mewnet_agent* agent) {
    if (agent == NULL) {
        return;
    }
    subs_mutex_lock(&agent->lock);
    agent->stop = true;
    subs_mutex_unlock(&agent->lock);
    mewnet_subs_cancel_fetch(agent->fetch_db);
    if (agent->fetch_thread_started) {
#ifdef _WIN32
        WaitForSingleObject(agent->fetch_thread, INFINITE);
        CloseHandle(agent->fetch_thread);
#else
        pthread_join(agent->fetch_thread, NULL);
#endif
        agent->fetch_thread_started = false;
    }
    mewnet_subs_close(agent->fetch_db);
    agent->fetch_db = NULL;
    if (agent->discovery_thread_started) {
#ifdef _WIN32
        WaitForSingleObject(agent->discovery_thread, INFINITE);
        CloseHandle(agent->discovery_thread);
#else
        pthread_join(agent->discovery_thread, NULL);
#endif
        agent->discovery_thread_started = false;
    }
    mewnet_subs_close(agent->discovery_db);
    agent->discovery_db = NULL;
    if (agent->thread_started) {
#ifdef _WIN32
        WaitForSingleObject(agent->thread, INFINITE);
        CloseHandle(agent->thread);
#else
        pthread_join(agent->thread, NULL);
#endif
        agent->thread_started = false;
    }
    subs_mutex_destroy(&agent->lock);
    free(agent);
}

void mewnet_agent_request_switch(mewnet_agent* agent) {
    if (agent == NULL) {
        return;
    }
    subs_mutex_lock(&agent->lock);
    agent->switch_requested = true;
    subs_mutex_unlock(&agent->lock);
}

void mewnet_agent_request_subscription(mewnet_agent* agent, int64_t subscription_id) {
    if (agent == NULL || subscription_id == 0) {
        return;
    }
    subs_mutex_lock(&agent->lock);
    agent->pending_fetch_id = subscription_id;
    subs_mutex_unlock(&agent->lock);
}

mewnet_agent_snapshot mewnet_agent_get_snapshot(mewnet_agent* agent) {
    mewnet_agent_snapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));
    if (agent == NULL) {
        return snapshot;
    }
    subs_mutex_lock(&agent->lock);
    snapshot.proxy_id = agent->current_id;
    snprintf(snapshot.name, sizeof(snapshot.name), "%s", agent->current_name);
    snprintf(snapshot.protocol, sizeof(snapshot.protocol), "%s", agent->current_protocol);
    snprintf(snapshot.server, sizeof(snapshot.server), "%s", agent->current_server);
    snapshot.port = agent->current_port;
    snapshot.delay_ms = agent->current_delay;
    snapshot.ready = agent->ready;
    subs_mutex_unlock(&agent->lock);
    agent_counts(agent, &snapshot.subscriptions, &snapshot.proxies,
                 &snapshot.alive, &snapshot.untested);
    return snapshot;
}
