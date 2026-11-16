/*
 * Subscription groups: what "the default group" is and how group_id = 0
 * means "all groups" for reads, counts, selection and batch testing.
 */

#include "mewnet_subs.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void check(int condition, const char* name) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static int64_t find_group(mewnet_subs* db, const char* name) {
    mewnet_subs_group_list groups = mewnet_subs_load_groups(db);
    int64_t id = 0;
    for (size_t i = 0; i < groups.count; i++) {
        if (strcmp(groups.items[i].name, name) == 0) {
            id = groups.items[i].id;
        }
    }
    mewnet_subs_unload_groups(groups);
    return id;
}

static int64_t first_proxy_id(mewnet_subs* db, int64_t group_id) {
    mewnet_subs_proxy_list list = mewnet_subs_load_proxies(db, group_id, 10, 0);
    int64_t id = list.count > 0 ? list.items[0].id : 0;
    mewnet_subs_unload_proxies(list);
    return id;
}

static mewnet_error_code add_proxy(mewnet_subs* db, int64_t group_id, const char* link) {
    return mewnet_subs_add_proxy(db, group_id, link).code;
}

int main(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mewnet-subs-groups-%ld.db", (long)getpid());
    unlink(path);

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = path;
    config.request_limit = 0;
    config.test_timeout_ms = 500;
    config.test_url = "http://127.0.0.1:9/204";

    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open: %s\n", opened.error.message);
        return 1;
    }
    mewnet_subs* db = opened.db;

    /* The default group is the first group in the database, created as "All". */
    int64_t all = 0;
    check(mewnet_subs_default_group(db, &all).code == MEWNET_ERROR_NONE && all > 0,
          "default group exists");
    check(mewnet_subs_add_group(db, "work", 50).code == MEWNET_ERROR_NONE, "add group");
    int64_t work = find_group(db, "work");
    check(work > 0 && work != all, "work group has its own id");
    check(all < work, "default group is the lowest id, not an aggregate");

    check(add_proxy(db, all,
                    "vless://11111111-2222-3333-4444-555555555555@127.0.0.1:1"
                    "?encryption=none&security=none&type=tcp#a") == MEWNET_ERROR_NONE,
          "add proxy a");
    check(add_proxy(db, work,
                    "vless://66666666-7777-8888-9999-000000000000@127.0.0.1:2"
                    "?encryption=none&security=none&type=tcp#b") == MEWNET_ERROR_NONE,
          "add proxy b");

    int64_t a = first_proxy_id(db, all);
    int64_t b = first_proxy_id(db, work);
    check(a > 0 && b > 0 && a != b, "one proxy in each group");

    check(mewnet_subs_record_test(db, a, true, 100, "").error.code == MEWNET_ERROR_NONE, "record a");
    check(mewnet_subs_record_test(db, b, true, 250, "").error.code == MEWNET_ERROR_NONE, "record b");

    int64_t n = 0;
    mewnet_subs_count_alive(db, all, &n);
    check(n == 1, "one alive in the default group");
    mewnet_subs_count_alive(db, work, &n);
    check(n == 1, "one alive in the work group");
    mewnet_subs_count_alive(db, 0, &n);
    check(n == 2, "group 0 counts every group");

    /* group_id = 0 is the "all subscriptions" view used for picking a node. */
    mewnet_subs_proxy first;
    check(mewnet_subs_first_alive(db, 0, &first).code == MEWNET_ERROR_NONE,
          "first alive across groups");
    check(first.id == a, "the faster proxy of both groups wins");

    mewnet_subs_proxy pick;
    check(mewnet_subs_random_best_alive(db, 0, 0, 0, 5, &pick).code == MEWNET_ERROR_NONE,
          "random best across groups");
    check(pick.id == a || pick.id == b, "the pick comes from one of the groups");

    mewnet_subs_proxy scoped;
    check(mewnet_subs_first_alive(db, work, &scoped).code == MEWNET_ERROR_NONE && scoped.id == b,
          "a real group id still filters");

    /* The scheduler treats 0 as "any subscription", not as the default group. */
    int64_t sub = -1;
    check(mewnet_subs_next_subscription_id(db, 0, true, &sub).code == MEWNET_ERROR_NOT_FOUND,
          "no subscriptions yet");
    check(mewnet_subs_add_subscription_in_group(db, work, "https://example.com/one", 0, "one")
                  .code == MEWNET_ERROR_NONE,
          "add subscription to the work group");
    check(mewnet_subs_next_subscription_id(db, 0, true, &sub).code == MEWNET_ERROR_NONE && sub > 0,
          "scheduler sees subscriptions in any group");

    /* Batch testing with group 0 covers configs from every group. */
    check(add_proxy(db, all,
                    "vless://aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee@127.0.0.1:3"
                    "?encryption=none&security=none&type=tcp#c") == MEWNET_ERROR_NONE,
          "add untested proxy c");
    check(add_proxy(db, work,
                    "vless://ffffffff-0000-1111-2222-333333333333@127.0.0.1:4"
                    "?encryption=none&security=none&type=tcp#d") == MEWNET_ERROR_NONE,
          "add untested proxy d");

    mewnet_config core_config = mewnet_config_default();
    core_config.mixed_port = -1;
    mewnet_init_result init = mewnet_init(&core_config);
    if (init.instance != NULL) {
        mewnet_subs_batch_test_result batch =
            mewnet_subs_test_batch(db, init.instance, 0, 10, 2);
        check(batch.error.code == MEWNET_ERROR_NONE, "test_batch accepts group 0");
        check(batch.tested >= 2, "test_batch with group 0 covers all groups");
        mewnet_close(init.instance);
    } else {
        fprintf(stderr, "skip: core init failed\n");
    }

    mewnet_subs_close(db);
    unlink(path);

    if (failures == 0) {
        printf("groups: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "groups: %d check(s) failed\n", failures);
    return 1;
}
