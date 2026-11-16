/*
 * mewnet-discovery - staged discovery probe.
 *
 * Fetches a set of subscriptions and then grows the pool of working proxies in
 * three stages, so the measured timings show how fast the first few nodes
 * appear, how long the first hundred take, and how the slow background pass
 * behaves afterwards.
 *
 * Stage 1 (fast):    wide batches until N working proxies are known.
 * Stage 2 (bulk):    smaller, paced batches until the target pool is reached.
 * Stage 3 (trickle): one small batch every few seconds in the background,
 *                    refreshing subscriptions when they are due.
 *
 * Usage:
 *   mewnet-discovery --db /tmp/disc.db \
 *     https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub
 */

#include "mewnet_subs.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void sleep_ms(int64_t ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts = {.tv_sec = (time_t)(ms / 1000),
                          .tv_nsec = (long)((ms % 1000) * 1000000)};
    nanosleep(&ts, NULL);
}

typedef struct Options {
    const char* db_path;
    size_t fast_target;
    size_t bulk_target;
    size_t fast_batch;
    size_t fast_concurrency;
    size_t bulk_batch;
    size_t bulk_concurrency;
    int bulk_pause_ms;
    int trickle_every_sec;
    size_t trickle_batch;
    int run_seconds;
    int refresh_sec;
    int verify;
    const char* json_path;
    const char** urls;
    int url_count;
} Options;

typedef struct Stats {
    size_t tested;
    size_t batches;
    int64_t first_alive_ms;
    int64_t fast_ms;
    int64_t bulk_ms;
    int64_t trickle_ms;
    size_t alive_after_fast;
    size_t alive_after_bulk;
    size_t alive_start;
    size_t alive_end;
    size_t fetch_added;
    size_t fetch_updated;
    size_t fetch_failed;
    int verify_status;
    unsigned verify_delay_ms;
} Stats;

typedef struct App {
    Options opt;
    Stats stats;
    mewnet_subs* db;
    mewnet_instance* core;
    int64_t start_ms;
    int64_t deadline_ms;
} App;

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [options] SUBSCRIPTION_URL...\n"
            "\n"
            "stages:\n"
            "  1 fast     wide batches until --fast working proxies are known\n"
            "  2 bulk     smaller paced batches until --bulk proxies are known\n"
            "  3 trickle  a small batch every --trickle-every seconds, forever\n"
            "\n"
            "options:\n"
            "  --db PATH            sqlite database (default data/discovery.db)\n"
            "  --fast N             stage 1 target (default 8)\n"
            "  --bulk N             stage 2 target (default 100)\n"
            "  --batch N            stage 1 batch size (default 64)\n"
            "  --concurrency N      stage 1 parallel checks (default 64)\n"
            "  --bulk-batch N       stage 2 batch size (default 16)\n"
            "  --bulk-concurrency N stage 2 parallel checks (default 8)\n"
            "  --bulk-pause MS      pause between stage 2 batches (default 250)\n"
            "  --trickle-every S    stage 3 interval (default 20)\n"
            "  --trickle-batch N    stage 3 batch size (default 4)\n"
            "  --refresh S          subscription refresh interval (default 3600)\n"
            "  --run-seconds N      stop after N seconds (0 = until Ctrl+C)\n"
            "  --no-verify          skip the probe through the best node\n"
            "  --json PATH          write a machine-readable summary\n",
            argv0);
}

static int take_value(int argc, char** argv, int* i, const char** out) {
    if (*i + 1 >= argc) {
        return 0;
    }
    *out = argv[++(*i)];
    return 1;
}

static int parse_args(int argc, char** argv, Options* opt) {
    memset(opt, 0, sizeof(*opt));
    opt->db_path = "data/discovery.db";
    opt->fast_target = 8;
    opt->bulk_target = 100;
    opt->fast_batch = 64;
    opt->fast_concurrency = 64;
    opt->bulk_batch = 16;
    opt->bulk_concurrency = 8;
    opt->bulk_pause_ms = 250;
    opt->trickle_every_sec = 20;
    opt->trickle_batch = 4;
    opt->refresh_sec = 3600;
    opt->verify = 1;

    const char* value = NULL;
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            exit(0);
        } else if (strcmp(arg, "--db") == 0 && take_value(argc, argv, &i, &value)) {
            opt->db_path = value;
        } else if (strcmp(arg, "--fast") == 0 && take_value(argc, argv, &i, &value)) {
            opt->fast_target = (size_t)atol(value);
        } else if (strcmp(arg, "--bulk") == 0 && take_value(argc, argv, &i, &value)) {
            opt->bulk_target = (size_t)atol(value);
        } else if (strcmp(arg, "--batch") == 0 && take_value(argc, argv, &i, &value)) {
            opt->fast_batch = (size_t)atol(value);
        } else if (strcmp(arg, "--concurrency") == 0 && take_value(argc, argv, &i, &value)) {
            opt->fast_concurrency = (size_t)atol(value);
        } else if (strcmp(arg, "--bulk-batch") == 0 && take_value(argc, argv, &i, &value)) {
            opt->bulk_batch = (size_t)atol(value);
        } else if (strcmp(arg, "--bulk-concurrency") == 0 && take_value(argc, argv, &i, &value)) {
            opt->bulk_concurrency = (size_t)atol(value);
        } else if (strcmp(arg, "--bulk-pause") == 0 && take_value(argc, argv, &i, &value)) {
            opt->bulk_pause_ms = atoi(value);
        } else if (strcmp(arg, "--trickle-every") == 0 && take_value(argc, argv, &i, &value)) {
            opt->trickle_every_sec = atoi(value);
        } else if (strcmp(arg, "--trickle-batch") == 0 && take_value(argc, argv, &i, &value)) {
            opt->trickle_batch = (size_t)atol(value);
        } else if (strcmp(arg, "--refresh") == 0 && take_value(argc, argv, &i, &value)) {
            opt->refresh_sec = atoi(value);
        } else if (strcmp(arg, "--run-seconds") == 0 && take_value(argc, argv, &i, &value)) {
            opt->run_seconds = atoi(value);
        } else if (strcmp(arg, "--json") == 0 && take_value(argc, argv, &i, &value)) {
            opt->json_path = value;
        } else if (strcmp(arg, "--no-verify") == 0) {
            opt->verify = 0;
        } else if (strncmp(arg, "--", 2) == 0) {
            fprintf(stderr, "unknown option %s\n", arg);
            return 0;
        } else {
            opt->urls = (const char**)&argv[i];
            opt->url_count = argc - i;
            break;
        }
    }
    if (opt->url_count == 0) {
        fprintf(stderr, "at least one subscription URL is required\n\n");
        usage(argv[0]);
        return 0;
    }
    if (opt->fast_target == 0) {
        opt->fast_target = 8;
    }
    if (opt->bulk_target < opt->fast_target) {
        opt->bulk_target = opt->fast_target;
    }
    return 1;
}

static size_t alive_now(App* app) {
    int64_t value = 0;
    if (mewnet_subs_count_alive(app->db, 0, &value).code != MEWNET_ERROR_NONE) {
        return 0;
    }
    return (size_t)value;
}

static size_t untested_now(App* app) {
    int64_t value = 0;
    if (mewnet_subs_count_untested(app->db, 0, &value).code != MEWNET_ERROR_NONE) {
        return 0;
    }
    return (size_t)value;
}

static size_t total_now(App* app) {
    int64_t value = 0;
    if (mewnet_subs_count_proxies(app->db, 0, &value).code != MEWNET_ERROR_NONE) {
        return 0;
    }
    return (size_t)value;
}

static int stop_requested(App* app) {
    if (g_stop) {
        return 1;
    }
    if (app->deadline_ms > 0 && now_ms() >= app->deadline_ms) {
        return 1;
    }
    return 0;
}

static void fetch_all(App* app) {
    mewnet_subscription_list subs = mewnet_subs_load_subscriptions(app->db);
    if (subs.error.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "subscriptions: %s\n", subs.error.message);
        return;
    }
    printf("== fetching %zu subscription(s)\n", subs.count);
    for (size_t i = 0; i < subs.count; i++) {
        if (stop_requested(app)) {
            break;
        }
        mewnet_subs_fetch_result r = mewnet_subs_fetch_now(app->db, subs.items[i].id);
        if (r.error.code != MEWNET_ERROR_NONE) {
            app->stats.fetch_failed++;
            printf("  fetch %s: %s\n", subs.items[i].url, r.error.message);
            continue;
        }
        app->stats.fetch_added += r.added;
        app->stats.fetch_updated += r.updated;
        printf("  fetch HTTP %d: +%zu ~%zu -%zu (%s)\n", r.http_status,
               r.added, r.updated, r.removed, subs.items[i].url);
    }
    mewnet_subs_unload_subscriptions(subs);
    printf("  configs in database: %zu\n", total_now(app));
    fflush(stdout);
}

/* Stage 1: wide and fast until a handful of nodes works. */
static void phase_fast(App* app) {
    int64_t t0 = now_ms();
    printf("== stage 1: first %zu working proxies (batch %zu, concurrency %zu)\n",
           app->opt.fast_target, app->opt.fast_batch, app->opt.fast_concurrency);
    fflush(stdout);

    while (!stop_requested(app)) {
        if (alive_now(app) >= app->opt.fast_target) {
            break;
        }
        mewnet_subs_batch_test_result batch =
            mewnet_subs_test_batch(app->db, app->core, 0, app->opt.fast_batch,
                                   app->opt.fast_concurrency);
        if (batch.error.code != MEWNET_ERROR_NONE) {
            fprintf(stderr, "  batch: %s\n", batch.error.message);
            break;
        }
        app->stats.batches++;
        app->stats.tested += batch.tested;
        size_t alive = alive_now(app);
        if (app->stats.first_alive_ms == 0 && alive > 0) {
            app->stats.first_alive_ms = now_ms() - app->start_ms;
        }
        printf("  %5.1fs tested=%zu alive=%zu untested=%zu\n",
               (double)(now_ms() - t0) / 1000.0, app->stats.tested, alive,
               untested_now(app));
        fflush(stdout);
        if (batch.tested == 0) {
            break;
        }
    }

    app->stats.fast_ms = now_ms() - t0;
    app->stats.alive_after_fast = alive_now(app);
    printf("== stage 1 done: %zu alive in %.1f s (first node after %.1f s)\n",
           app->stats.alive_after_fast, (double)app->stats.fast_ms / 1000.0,
           (double)app->stats.first_alive_ms / 1000.0);
    fflush(stdout);
}

/* Stage 2: grow to the bulk target with smaller, paced batches. */
static void phase_bulk(App* app) {
    int64_t t0 = now_ms();
    printf("== stage 2: grow to %zu proxies (batch %zu, concurrency %zu, pause %d ms)\n",
           app->opt.bulk_target, app->opt.bulk_batch, app->opt.bulk_concurrency,
           app->opt.bulk_pause_ms);
    fflush(stdout);

    while (!stop_requested(app)) {
        if (alive_now(app) >= app->opt.bulk_target) {
            break;
        }
        mewnet_subs_batch_test_result batch =
            mewnet_subs_test_batch(app->db, app->core, 0, app->opt.bulk_batch,
                                   app->opt.bulk_concurrency);
        if (batch.error.code != MEWNET_ERROR_NONE) {
            fprintf(stderr, "  batch: %s\n", batch.error.message);
            break;
        }
        app->stats.batches++;
        app->stats.tested += batch.tested;
        size_t alive = alive_now(app);
        printf("  %5.1fs tested=%zu alive=%zu untested=%zu\n",
               (double)(now_ms() - t0) / 1000.0, app->stats.tested, alive,
               untested_now(app));
        fflush(stdout);
        if (batch.tested == 0) {
            break;
        }
        sleep_ms(app->opt.bulk_pause_ms);
    }

    app->stats.bulk_ms = now_ms() - t0;
    app->stats.alive_after_bulk = alive_now(app);
    printf("== stage 2 done: %zu alive in %.1f s\n", app->stats.alive_after_bulk,
           (double)app->stats.bulk_ms / 1000.0);
    fflush(stdout);
}

static void refresh_due(App* app) {
    mewnet_subscription_list subs = mewnet_subs_load_subscriptions(app->db);
    if (subs.error.code != MEWNET_ERROR_NONE) {
        return;
    }
    int64_t now = now_ms();
    for (size_t i = 0; i < subs.count; i++) {
        int64_t last = subs.items[i].last_fetch_at;
        if (last > 0 && now - last < (int64_t)app->opt.refresh_sec * 1000) {
            continue;
        }
        mewnet_subs_fetch_result r = mewnet_subs_fetch_now(app->db, subs.items[i].id);
        if (r.error.code == MEWNET_ERROR_NONE) {
            app->stats.fetch_added += r.added;
            app->stats.fetch_updated += r.updated;
            printf("  refresh: +%zu ~%zu (HTTP %d)\n", r.added, r.updated, r.http_status);
        }
    }
    mewnet_subs_unload_subscriptions(subs);
}

/* Stage 3: slow background pass, one small batch at a time. */
static void phase_trickle(App* app) {
    int64_t t0 = now_ms();
    int64_t next_tick = t0;
    int64_t next_refresh = t0 + (int64_t)app->opt.refresh_sec * 1000;
    printf("== stage 3: trickle %zu config(s) every %d s in the background\n",
           app->opt.trickle_batch, app->opt.trickle_every_sec);
    fflush(stdout);

    while (!stop_requested(app)) {
        int64_t now = now_ms();
        if (now >= next_refresh) {
            refresh_due(app);
            next_refresh = now + (int64_t)app->opt.refresh_sec * 1000;
        }
        if (now >= next_tick) {
            mewnet_subs_batch_test_result batch =
                mewnet_subs_test_batch(app->db, app->core, 0, app->opt.trickle_batch,
                                       app->opt.trickle_batch);
            if (batch.error.code == MEWNET_ERROR_NONE) {
                app->stats.batches++;
                app->stats.tested += batch.tested;
                printf("  %6.1fs trickle tested=%zu alive=%zu (total tested %zu)\n",
                       (double)(now_ms() - t0) / 1000.0, batch.tested, alive_now(app),
                       app->stats.tested);
                fflush(stdout);
            }
            next_tick = now_ms() + (int64_t)app->opt.trickle_every_sec * 1000;
        }
        sleep_ms(200);
    }

    app->stats.trickle_ms = now_ms() - t0;
    app->stats.alive_end = alive_now(app);
    printf("== stage 3 done after %.1f s: %zu alive\n",
           (double)app->stats.trickle_ms / 1000.0, app->stats.alive_end);
    fflush(stdout);
}

static int compare_unsigned(const void* a, const void* b) {
    unsigned x = *(const unsigned*)a;
    unsigned y = *(const unsigned*)b;
    return (x > y) - (x < y);
}

static void report(App* app) {
    mewnet_subs_stats stats = mewnet_subs_get_stats(app->db);
    printf("\n== results\n");
    printf("subscriptions       : %lld\n", (long long)stats.subscriptions);
    printf("configs stored      : %lld\n", (long long)stats.proxies);
    printf("checks executed     : %zu in %zu batches\n", app->stats.tested, app->stats.batches);
    printf("first working node  : %.2f s\n", (double)app->stats.first_alive_ms / 1000.0);
    printf("stage 1 (%2zu nodes) : %6.2f s -> %zu alive\n", app->opt.fast_target,
           (double)app->stats.fast_ms / 1000.0, app->stats.alive_after_fast);
    printf("stage 2 (%3zu nodes): %6.2f s -> %zu alive\n", app->opt.bulk_target,
           (double)app->stats.bulk_ms / 1000.0, app->stats.alive_after_bulk);
    printf("stage 3 (trickle)   : %6.2f s -> %zu alive\n",
           (double)app->stats.trickle_ms / 1000.0, app->stats.alive_end);
    if (stats.alive + stats.dead > 0) {
        printf("alive vs dead (db)  : %lld / %lld (%.1f%%)\n", (long long)stats.alive,
               (long long)stats.dead,
               100.0 * (double)stats.alive / (double)(stats.alive + stats.dead));
    }
    if (app->stats.tested > 0) {
        size_t gained = app->stats.alive_end > app->stats.alive_start
                            ? app->stats.alive_end - app->stats.alive_start
                            : 0;
        printf("new nodes per check : %.1f%% (%zu of %zu checks)\n",
               100.0 * (double)gained / (double)app->stats.tested, gained,
               app->stats.tested);
    }

    /* Walk every page: the alive configs are spread over the whole table. */
    size_t alive_total = alive_now(app);
    unsigned* delays = (unsigned*)calloc(alive_total > 0 ? alive_total : 1, sizeof(unsigned));
    size_t delay_count = 0;
    /* Copy the names: the page buffer they point into is freed below. */
    char protocols[16][MEWNET_TYPE_MAX];
    size_t counts[16];
    size_t protocol_count = 0;
    mewnet_subs_proxy best;
    int have_best = 0;

    size_t offset = 0;
    for (;;) {
        mewnet_subs_proxy_list list = mewnet_subs_load_proxies(app->db, 0, 1000, offset);
        if (list.error.code != MEWNET_ERROR_NONE || list.count == 0) {
            mewnet_subs_unload_proxies(list);
            break;
        }
        for (size_t i = 0; i < list.count; i++) {
            mewnet_subs_proxy* p = &list.items[i];
            if (!p->alive) {
                continue;
            }
            size_t slot = protocol_count;
            for (size_t k = 0; k < protocol_count; k++) {
                if (strcmp(protocols[k], p->protocol) == 0) {
                    slot = k;
                    break;
                }
            }
            if (slot == protocol_count && protocol_count < 16) {
                snprintf(protocols[protocol_count], MEWNET_TYPE_MAX, "%s", p->protocol);
                counts[protocol_count] = 0;
                slot = protocol_count++;
            }
            if (slot < protocol_count) {
                counts[slot]++;
            }
            if (delays != NULL && p->last_delay_ms > 0 && delay_count < alive_total) {
                delays[delay_count++] = p->last_delay_ms;
            }
            if (!have_best || (p->last_delay_ms > 0 &&
                               (best.last_delay_ms == 0 || p->last_delay_ms < best.last_delay_ms))) {
                best = *p;
                have_best = 1;
            }
        }
        size_t got = list.count;
        mewnet_subs_unload_proxies(list);
        offset += got;
        if (got < 1000) {
            break;
        }
    }

    printf("protocols           :");
    for (size_t k = 0; k < protocol_count; k++) {
        printf(" %s=%zu", protocols[k], counts[k]);
    }
    printf("\n");
    if (delay_count > 0 && delays != NULL) {
        qsort(delays, delay_count, sizeof(unsigned), compare_unsigned);
        printf("delay p50/p90       : %u / %u ms\n", delays[delay_count / 2],
               delays[(delay_count * 9) / 10]);
    }
    if (have_best && best.last_delay_ms > 0) {
        printf("best node           : %s (%s) %u ms\n", best.name, best.protocol,
               best.last_delay_ms);
    }
    free(delays);

    if (app->opt.verify && app->stats.alive_end > 0) {
        mewnet_subs_proxy chosen;
        if (mewnet_subs_random_best_alive(app->db, 0, 0, 0, 5, &chosen).code ==
            MEWNET_ERROR_NONE) {
            mewnet_add_options add = mewnet_add_options_default();
            add.select_proxy = true;
            if (mewnet_add_proxy(app->core, chosen.link, &add) == MEWNET_ERROR_NONE) {
                mewnet_probe_result probe = mewnet_probe(
                    app->core, "https://www.gstatic.com/generate_204", 204, 8000);
                app->stats.verify_status = probe.status;
                app->stats.verify_delay_ms = probe.delay_ms;
                printf("verify through      : %s -> HTTP %d in %u ms\n", chosen.name,
                       probe.status, probe.delay_ms);
            }
        }
    }
    printf("\n");
    fflush(stdout);
}

static void write_json(App* app) {
    if (app->opt.json_path == NULL) {
        return;
    }
    FILE* f = fopen(app->opt.json_path, "w");
    if (f == NULL) {
        fprintf(stderr, "json: cannot write %s\n", app->opt.json_path);
        return;
    }
    mewnet_subs_stats stats = mewnet_subs_get_stats(app->db);
    fprintf(f,
            "{\n"
            "  \"configs\": %lld,\n"
            "  \"subscriptions\": %lld,\n"
            "  \"checks\": %zu,\n"
            "  \"batches\": %zu,\n"
            "  \"first_alive_ms\": %lld,\n"
            "  \"stage1_ms\": %lld,\n"
            "  \"stage1_alive\": %zu,\n"
            "  \"stage1_target\": %zu,\n"
            "  \"stage2_ms\": %lld,\n"
            "  \"stage2_alive\": %zu,\n"
            "  \"stage2_target\": %zu,\n"
            "  \"stage3_ms\": %lld,\n"
            "  \"stage3_alive\": %zu,\n"
            "  \"verify_status\": %d,\n"
            "  \"verify_delay_ms\": %u\n"
            "}\n",
            (long long)stats.proxies, (long long)stats.subscriptions, app->stats.tested,
            app->stats.batches, (long long)app->stats.first_alive_ms,
            (long long)app->stats.fast_ms, app->stats.alive_after_fast,
            app->opt.fast_target, (long long)app->stats.bulk_ms,
            app->stats.alive_after_bulk, app->opt.bulk_target,
            (long long)app->stats.trickle_ms, app->stats.alive_end,
            app->stats.verify_status, app->stats.verify_delay_ms);
    fclose(f);
    printf("json written to %s\n", app->opt.json_path);
}

int main(int argc, char** argv) {
    App app;
    memset(&app, 0, sizeof(app));
    if (!parse_args(argc, argv, &app.opt)) {
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    app.start_ms = now_ms();
    if (app.opt.run_seconds > 0) {
        app.deadline_ms = app.start_ms + (int64_t)app.opt.run_seconds * 1000;
    }

    char* path_copy = strdup(app.opt.db_path);
    if (path_copy != NULL) {
        char* slash = strrchr(path_copy, '/');
        if (slash != NULL) {
            *slash = '\0';
            if (path_copy[0] != '\0') {
                mkdir(path_copy, 0755);
            }
        }
        free(path_copy);
    }

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = app.opt.db_path;
    config.request_limit = 0; /* the stages pace themselves */
    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open database: %s\n", opened.error.message);
        return 1;
    }
    app.db = opened.db;

    int64_t group = 0;
    mewnet_error err = mewnet_subs_default_group(app.db, &group);
    if (err.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "default group: %s\n", err.message);
        mewnet_subs_close(app.db);
        return 1;
    }
    for (int i = 0; i < app.opt.url_count; i++) {
        err = mewnet_subs_add_subscription_in_group(app.db, group, app.opt.urls[i], 0, NULL);
        if (err.code != MEWNET_ERROR_NONE) {
            fprintf(stderr, "add %s: %s\n", app.opt.urls[i], err.message);
        }
    }

    mewnet_config core_config = mewnet_config_default();
    core_config.mixed_port = 0; /* the probe goes through the local inbound */
    core_config.log_level = "error";
    mewnet_init_result init = mewnet_init(&core_config);
    if (init.instance == NULL) {
        fprintf(stderr, "core init: %s\n", init.error.message);
        mewnet_subs_close(app.db);
        return 1;
    }
    app.core = init.instance;

    fetch_all(&app);
    app.stats.alive_start = alive_now(&app);
    if (!stop_requested(&app)) {
        phase_fast(&app);
    }
    if (!stop_requested(&app)) {
        phase_bulk(&app);
    }
    if (!stop_requested(&app)) {
        phase_trickle(&app);
    }

    app.stats.alive_end = alive_now(&app);
    report(&app);
    write_json(&app);

    mewnet_close(app.core);
    mewnet_subs_close(app.db);
    return 0;
}
