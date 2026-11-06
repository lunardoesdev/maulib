#include "mewnet_subs.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [options] SUBSCRIPTION_URL...\n"
            "\n"
            "options:\n"
            "  --db PATH          sqlite database (default data/mewnet-watch.db)\n"
            "  --proxy-port N     mixed proxy port (default 0 = free port)\n"
            "  --batch N          proxies per discovery batch (default 70)\n"
            "  --concurrency N    concurrent checks per batch (default 70)\n"
            "  --test-url URL     URL used for proxy checks\n"
            "  --timeout MS       per-proxy timeout (default 4000)\n"
            "  --fetch-interval S subscription refresh interval (default 3600)\n",
            argv0);
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int fetch_next_subscription(mewnet_subs* db, int64_t group_id, int64_t* last_fetch_ms) {
    mewnet_subscription_list list = mewnet_subs_load_subscriptions_in_group(db, group_id);
    if (list.error.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "load subscriptions: %s\n", list.error.message);
        return -1;
    }
    if (list.count == 0) {
        mewnet_subs_unload_subscriptions(list);
        return -1;
    }
    int64_t best_id = 0;
    int64_t best_last = 0;
    for (size_t i = 0; i < list.count; i++) {
        int64_t last = list.items[i].last_fetch_at;
        if (best_id == 0 || last < best_last) {
            best_id = list.items[i].id;
            best_last = last;
        }
    }
    mewnet_subs_unload_subscriptions(list);
    if (best_id == 0) {
        return -1;
    }
    mewnet_subs_fetch_result fetched = mewnet_subs_fetch_now(db, best_id);
    if (fetched.error.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "fetch %lld: %s\n", (long long)best_id, fetched.error.message);
        return -1;
    }
    *last_fetch_ms = now_ms();
    printf("FETCH t=%lld id=%lld status=%d added=%zu updated=%zu removed=%zu\n",
           (long long)now_ms(),
           (long long)fetched.subscription_id, fetched.http_status,
           fetched.added, fetched.updated, fetched.removed);
    fflush(stdout);
    return 0;
}

static int route_first_alive(mewnet_subs* db, mewnet_instance* milib, int64_t group_id) {
    mewnet_subs_proxy proxy;
    mewnet_error err = mewnet_subs_first_alive(db, group_id, &proxy);
    if (err.code != MEWNET_ERROR_NONE) {
        return err.code == MEWNET_ERROR_NOT_FOUND ? 0 : -1;
    }
    mewnet_add_options options = mewnet_add_options_default();
    options.select_proxy = true;
    mewnet_error_code add_code = mewnet_add_proxy(milib, proxy.link, &options);
    if (add_code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "route proxy: code %d\n", add_code);
        return -1;
    }
    mewnet_status_info status = mewnet_get_status(milib);
    printf("WORKING id=%lld name=%s\n", (long long)proxy.id,
           proxy.name[0] != '\0' ? proxy.name : proxy.link);
    printf("PROXY http://127.0.0.1:%d\n", status.mixed_port);
    printf("READY\n");
    fflush(stdout);
    return 1;
}

int main(int argc, char** argv) {
    const char* db_path = "data/mewnet-watch.db";
    int proxy_port = 0;
    size_t batch = 70;
    size_t concurrency = 70;
    const char* test_url = NULL;
    int timeout_ms = 4000;
    int fetch_interval = 3600;
    int first_url = argc;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
        } else if (strcmp(argv[i], "--proxy-port") == 0 && i + 1 < argc) {
            proxy_port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--batch") == 0 && i + 1 < argc) {
            batch = (size_t)atol(argv[++i]);
        } else if (strcmp(argv[i], "--concurrency") == 0 && i + 1 < argc) {
            concurrency = (size_t)atol(argv[++i]);
        } else if (strcmp(argv[i], "--test-url") == 0 && i + 1 < argc) {
            test_url = argv[++i];
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            timeout_ms = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fetch-interval") == 0 && i + 1 < argc) {
            fetch_interval = atoi(argv[++i]);
        } else if (strncmp(argv[i], "--", 2) == 0) {
            usage(argv[0]);
            return 2;
        } else {
            first_url = i;
            break;
        }
    }
    if (first_url >= argc) {
        usage(argv[0]);
        return 2;
    }

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = db_path;
    config.test_url = test_url;
    config.test_timeout_ms = timeout_ms;
    config.request_limit = 10;
    config.request_window_seconds = 240;
    config.default_fetch_interval_sec = fetch_interval;
    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open db: %s\n", opened.error.message);
        return 1;
    }
    mewnet_subs* db = opened.db;

    int priority = 1000;
    for (int i = first_url; i < argc; i++) {
        mewnet_error err = mewnet_subs_add_subscription(db, argv[i], priority--, NULL);
        if (err.code != MEWNET_ERROR_NONE) {
            fprintf(stderr, "add subscription: %s\n", err.message);
            return 1;
        }
    }

    int64_t group_id = 0;
    mewnet_error err = mewnet_subs_default_group(db, &group_id);
    if (err.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "default group: %s\n", err.message);
        return 1;
    }

    int64_t last_fetch_ms = 0;
    mewnet_subscription_list existing = mewnet_subs_load_subscriptions_in_group(db, group_id);
    if (existing.error.code == MEWNET_ERROR_NONE) {
        for (size_t i = 0; i < existing.count; i++) {
            if (existing.items[i].last_fetch_at > last_fetch_ms) {
                last_fetch_ms = existing.items[i].last_fetch_at;
            }
        }
        mewnet_subs_unload_subscriptions(existing);
    }

    mewnet_config milib_config = mewnet_config_default();
    milib_config.mixed_port = proxy_port;
    milib_config.log_level = "warning";
    mewnet_init_result init = mewnet_init(&milib_config);
    if (init.instance == NULL) {
        fprintf(stderr, "milib init: %s\n", init.error.message);
        return 1;
    }
    mewnet_instance* milib = init.instance;

    mewnet_status_info status = mewnet_get_status(milib);
    printf("DB %s\n", db_path);
    printf("PROXY http://127.0.0.1:%d\n", status.mixed_port);
    fflush(stdout);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int routed = 0;
    while (!g_stop) {
        int64_t alive = 0;
        int64_t untested = 0;
        mewnet_subs_count_alive(db, group_id, &alive);
        mewnet_subs_count_untested(db, group_id, &untested);

        if (alive == 0) {
            if (untested == 0) {
                fetch_next_subscription(db, group_id, &last_fetch_ms);
                struct timespec pause = {.tv_sec = 2, .tv_nsec = 0};
                nanosleep(&pause, NULL);
                continue;
            }
            mewnet_subs_batch_test_result tested = mewnet_subs_test_batch(
                db, milib, group_id, batch, concurrency);
            if (tested.error.code != MEWNET_ERROR_NONE) {
                fprintf(stderr, "batch: %s\n", tested.error.message);
                struct timespec pause = {.tv_sec = 2, .tv_nsec = 0};
                nanosleep(&pause, NULL);
                continue;
            }
            printf("BATCH t=%lld tested=%zu alive=%zu\n",
                   (long long)now_ms(), tested.tested, tested.alive);
            fflush(stdout);
            if (tested.alive > 0) {
                routed = route_first_alive(db, milib, group_id);
            }
            continue;
        }

        if (!routed) {
            routed = route_first_alive(db, milib, group_id);
            if (routed == 1) {
                continue;
            }
        }

        if (last_fetch_ms == 0 || now_ms() - last_fetch_ms >= (int64_t)fetch_interval * 1000) {
            mewnet_subs_fetch_result due = mewnet_subs_fetch_due(db);
            if (due.error.code == MEWNET_ERROR_NONE && due.subscription_id != 0) {
                last_fetch_ms = now_ms();
                printf("FETCH t=%lld id=%lld status=%d added=%zu updated=%zu removed=%zu\n",
           (long long)now_ms(),
                       (long long)due.subscription_id, due.http_status,
                       due.added, due.updated, due.removed);
                fflush(stdout);
                continue;
            }
        }

        mewnet_subs_test_result one = mewnet_subs_test_next(db, milib);
        if (one.error.code == MEWNET_ERROR_NONE) {
            printf("TEST t=%lld id=%lld alive=%d delay=%u rating=%d->%d\n",
                   (long long)now_ms(), (long long)one.proxy_id,
                   one.alive ? 1 : 0, one.delay_ms,
                   one.rating_before, one.rating_after);
            fflush(stdout);
        } else if (one.error.code == MEWNET_ERROR_NOT_FOUND) {
            int64_t alive_now = 0;
            mewnet_subs_count_alive(db, group_id, &alive_now);
            if (alive_now == 0) {
                fetch_next_subscription(db, group_id, &last_fetch_ms);
            }
            struct timespec pause = {.tv_sec = 5, .tv_nsec = 0};
            nanosleep(&pause, NULL);
        } else {
            fprintf(stderr, "test: %s\n", one.error.message);
            struct timespec pause = {.tv_sec = 2, .tv_nsec = 0};
            nanosleep(&pause, NULL);
        }
    }

    mewnet_close(milib);
    mewnet_subs_close(db);
    return 0;
}
