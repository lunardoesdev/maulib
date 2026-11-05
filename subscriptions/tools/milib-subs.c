#include "milib_subs.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static void print_error(const char* operation, milib_error error) {
    fprintf(stderr, "%s: %s (code %d)\n", operation,
            error.message[0] != '\0' ? error.message : "unknown error", error.code);
}

static int print_fetch(const milib_subs_fetch_result* result) {
    if (result->error.code != MILIB_ERROR_NONE) {
        print_error("fetch", result->error);
        return 1;
    }
    if (result->subscription_id == 0) {
        printf("fetch: nothing due\n");
        return 0;
    }
    printf("fetch: subscription=%lld status=%d added=%zu updated=%zu removed=%zu skipped=%zu\n",
           (long long)result->subscription_id, result->http_status,
           result->added, result->updated, result->removed, result->skipped);
    return 0;
}

static int print_test(const milib_subs_test_result* result) {
    if (result->error.code != MILIB_ERROR_NONE) {
        print_error("test", result->error);
        return 1;
    }
    printf("test: proxy=%lld alive=%d delay=%ums rating=%d->%d (%+d) %s\n",
           (long long)result->proxy_id, result->alive ? 1 : 0,
           result->delay_ms, result->rating_before, result->rating_after,
           result->rating_delta, result->name);
    if (!result->alive && result->test_error[0] != '\0') {
        printf("test error: %s\n", result->test_error);
    }
    return 0;
}

static void usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s --db PATH <command> [args]\n"
            "\n"
            "global options:\n"
            "  --db PATH        sqlite database file\n"
            "  --limit N        shared request limit (default 10)\n"
            "  --window SEC     rate-limit window (default 240)\n"
            "  --test-url URL   proxy test URL\n"
            "  --timeout MS     proxy test timeout (default 5000)\n"
            "\n"
            "commands:\n"
            "  sub add URL [priority] [name]\n"
            "  sub rm ID\n"
            "  sub priority ID N\n"
            "  sub enable ID 0|1\n"
            "  sub ls\n"
            "  fetch ID\n"
            "  fetch --due\n"
            "  test [ID]\n"
            "  run\n"
            "  proxies [--sub ID] [--limit N] [--offset N]\n"
            "  stats\n",
            argv0);
}

static milib_instance* init_milib(void) {
    milib_config config = milib_config_default();
    config.mixed_port = -1;
    config.log_level = "warning";
    milib_init_result result = milib_init(&config);
    if (result.instance == NULL) {
        print_error("milib init", result.error);
        return NULL;
    }
    return result.instance;
}

static int command_sub(milib_subs* db, int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[1], "add") == 0) {
        if (argc < 3) return 2;
        int priority = argc > 3 ? atoi(argv[3]) : 0;
        const char* name = argc > 4 ? argv[4] : "";
        milib_error err = milib_subs_add_subscription(db, argv[2], priority, name);
        if (err.code != MILIB_ERROR_NONE) {
            print_error("sub add", err);
            return 1;
        }
        printf("subscription added\n");
        return 0;
    }
    if (strcmp(argv[1], "rm") == 0) {
        if (argc < 3) return 2;
        milib_error err = milib_subs_remove_subscription(db, atoll(argv[2]));
        if (err.code != MILIB_ERROR_NONE) {
            print_error("sub rm", err);
            return 1;
        }
        printf("subscription removed\n");
        return 0;
    }
    if (strcmp(argv[1], "priority") == 0) {
        if (argc < 4) return 2;
        milib_error err = milib_subs_set_priority(db, atoll(argv[2]), atoi(argv[3]));
        if (err.code != MILIB_ERROR_NONE) {
            print_error("sub priority", err);
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "enable") == 0) {
        if (argc < 4) return 2;
        milib_error err = milib_subs_set_enabled(db, atoll(argv[2]), atoi(argv[3]) != 0);
        if (err.code != MILIB_ERROR_NONE) {
            print_error("sub enable", err);
            return 1;
        }
        return 0;
    }
    if (strcmp(argv[1], "ls") == 0) {
        milib_subscription_list list = milib_subs_load_subscriptions(db);
        if (list.error.code != MILIB_ERROR_NONE) {
            print_error("sub ls", list.error);
            return 1;
        }
        for (size_t i = 0; i < list.count; i++) {
            const milib_subscription* item = &list.items[i];
            printf("%lld priority=%d enabled=%d proxies-fetch=%lld next=%lld name=%s\n",
                   (long long)item->id, item->priority, item->enabled ? 1 : 0,
                   (long long)item->last_fetch_at, (long long)item->next_fetch_at,
                   item->name);
            printf("  %s\n", item->url);
            if (item->last_error[0] != '\0') {
                printf("  error: %s\n", item->last_error);
            }
        }
        milib_subs_unload_subscriptions(list);
        return 0;
    }
    usage(argv[0]);
    return 2;
}

static int command_run(milib_subs* db) {
    milib_instance* instance = init_milib();
    if (instance == NULL) {
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int exit_code = 0;
    while (!g_stop) {
        milib_subs_fetch_result fetched = milib_subs_fetch_due(db);
        if (fetched.error.code == MILIB_ERROR_NONE && fetched.subscription_id != 0) {
            print_fetch(&fetched);
            continue;
        }
        if (fetched.error.code != MILIB_ERROR_NONE) {
            print_fetch(&fetched);
        }

        milib_subs_test_result tested = milib_subs_test_next(db, instance);
        if (tested.error.code == MILIB_ERROR_NONE) {
            print_test(&tested);
            continue;
        }
        if (tested.error.code == MILIB_ERROR_NOT_FOUND) {
            sleep(5);
            continue;
        }
        print_test(&tested);
        exit_code = 1;
        sleep(5);
    }

    milib_close(instance);
    return exit_code;
}

int main(int argc, char** argv) {
    const char* db_path = NULL;
    milib_subs_config config = milib_subs_config_default();

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_path = argv[++i];
            i++;
        } else if (strcmp(argv[i], "--limit") == 0 && i + 1 < argc) {
            config.request_limit = atoi(argv[++i]);
            i++;
        } else if (strcmp(argv[i], "--window") == 0 && i + 1 < argc) {
            config.request_window_seconds = atoi(argv[++i]);
            i++;
        } else if (strcmp(argv[i], "--test-url") == 0 && i + 1 < argc) {
            config.test_url = argv[++i];
            i++;
        } else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
            config.test_timeout_ms = atoi(argv[++i]);
            i++;
        } else {
            break;
        }
    }

    if (db_path == NULL || i >= argc) {
        usage(argv[0]);
        return 2;
    }
    config.db_path = db_path;

    milib_subs_open_result opened = milib_subs_open(&config);
    if (opened.db == NULL) {
        print_error("open", opened.error);
        return 1;
    }
    milib_subs* db = opened.db;

    const char* command = argv[i++];
    int rc = 0;
    if (strcmp(command, "sub") == 0) {
        rc = command_sub(db, argc - i + 1, argv + i - 1);
    } else if (strcmp(command, "fetch") == 0) {
        if (i < argc && strcmp(argv[i], "--due") == 0) {
            milib_subs_fetch_result result = milib_subs_fetch_due(db);
            rc = print_fetch(&result);
        } else if (i < argc) {
            milib_subs_fetch_result result = milib_subs_fetch(db, atoll(argv[i]));
            rc = print_fetch(&result);
        } else {
            usage(argv[0]);
            rc = 2;
        }
    } else if (strcmp(command, "test") == 0) {
        milib_instance* instance = init_milib();
        if (instance == NULL) {
            rc = 1;
        } else {
            milib_subs_test_result result;
            if (i < argc) {
                result = milib_subs_test_proxy(db, instance, atoll(argv[i]));
            } else {
                result = milib_subs_test_next(db, instance);
            }
            rc = print_test(&result);
            milib_close(instance);
        }
    } else if (strcmp(command, "run") == 0) {
        rc = command_run(db);
    } else if (strcmp(command, "proxies") == 0) {
        int64_t subscription_id = 0;
        size_t limit = 100;
        size_t offset = 0;
        for (int j = i; j < argc; j++) {
            if (strcmp(argv[j], "--sub") == 0 && j + 1 < argc) {
                subscription_id = atoll(argv[++j]);
            } else if (strcmp(argv[j], "--limit") == 0 && j + 1 < argc) {
                limit = (size_t)atol(argv[++j]);
            } else if (strcmp(argv[j], "--offset") == 0 && j + 1 < argc) {
                offset = (size_t)atol(argv[++j]);
            }
        }
        milib_subs_proxy_list list =
            milib_subs_load_proxies(db, subscription_id, limit, offset);
        if (list.error.code != MILIB_ERROR_NONE) {
            print_error("proxies", list.error);
            rc = 1;
        } else {
            for (size_t j = 0; j < list.count; j++) {
                const milib_subs_proxy* item = &list.items[j];
                printf("%lld sub=%lld rating=%d alive=%d delay=%u %s %s:%d\n",
                       (long long)item->id, (long long)item->subscription_id,
                       item->rating, item->alive ? 1 : 0, item->last_delay_ms,
                       item->name[0] != '\0' ? item->name : item->link,
                       item->server, item->port);
            }
            milib_subs_unload_proxies(list);
        }
    } else if (strcmp(command, "stats") == 0) {
        milib_subs_stats stats = milib_subs_get_stats(db);
        printf("subscriptions=%lld proxies=%lld alive=%lld dead=%lld tests=%lld\n",
               (long long)stats.subscriptions, (long long)stats.proxies,
               (long long)stats.alive, (long long)stats.dead,
               (long long)stats.tests);
    } else {
        usage(argv[0]);
        rc = 2;
    }

    milib_subs_close(db);
    return rc;
}
