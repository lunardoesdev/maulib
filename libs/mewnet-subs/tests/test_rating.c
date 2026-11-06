#include "mewnet_subs_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void check(int condition, const char* name) {
    if (!condition) {
        fprintf(stderr, "FAIL %s\n", name);
        failures++;
    }
}

static int query_int(sqlite3* db, const char* sql) {
    sqlite3_stmt* stmt = NULL;
    int value = -999;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

int main(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mewnet-subs-rating-%ld.db", (long)getpid());
    unlink(path);

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = path;
    config.request_limit = 0;
    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open: %s\n", opened.error.message);
        return 1;
    }
    mewnet_subs* db = opened.db;

    mewnet_error err = mewnet_subs_add_subscription(db, "https://example.test/sub", 10, "test");
    if (err.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "add: %s\n", err.message);
        return 1;
    }

    sqlite3* raw = NULL;
    if (sqlite3_open(path, &raw) != SQLITE_OK) {
        fprintf(stderr, "raw open failed\n");
        return 1;
    }
    int sub_id = query_int(raw, "SELECT id FROM subscriptions LIMIT 1");
    int group_id = query_int(raw, "SELECT group_id FROM subscriptions LIMIT 1");
    err = mewnet_subs_add_proxy(db, group_id, "vless://x");
    if (err.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "seed proxy failed: %s\n", err.message);
        return 1;
    }
    int proxy_id = query_int(raw, "SELECT id FROM proxies LIMIT 1");

    mewnet_subs_test_result r;

    r = mewnet_subs_record_test(db, proxy_id, true, 42, "");
    check(r.error.code == MEWNET_ERROR_NONE && r.rating_delta == 1 && r.rating_after == 1, "rating +1");
    int64_t untested = -1;
    int64_t alive_count = -1;
    check(mewnet_subs_count_untested(db, group_id, &untested).code == MEWNET_ERROR_NONE && untested == 0,
          "count untested");
    check(mewnet_subs_count_alive(db, group_id, &alive_count).code == MEWNET_ERROR_NONE && alive_count == 1,
          "count alive");
    mewnet_subs_proxy first_alive;
    check(mewnet_subs_first_alive(db, group_id, &first_alive).code == MEWNET_ERROR_NONE &&
          first_alive.id == proxy_id, "first alive");
    mewnet_subs_proxy next;
    check(mewnet_subs_next_alive(db, group_id, proxy_id, &next).code == MEWNET_ERROR_NOT_FOUND,
          "next alive empty");
    err = mewnet_subs_add_proxy(db, group_id,
        "vless://aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee@127.0.0.1:8?encryption=none&security=none&type=tcp#temp");
    check(err.code == MEWNET_ERROR_NONE, "add temp");
    int temp_id = query_int(raw, "SELECT id FROM proxies WHERE name='temp' LIMIT 1");
    r = mewnet_subs_record_test(db, temp_id, true, 55, "");
    check(r.error.code == MEWNET_ERROR_NONE && r.alive, "temp alive");
    check(mewnet_subs_next_alive(db, group_id, proxy_id, &next).code == MEWNET_ERROR_NONE &&
          next.id == temp_id, "next alive after current");
    check(mewnet_subs_next_alive(db, group_id, temp_id, &next).code == MEWNET_ERROR_NONE &&
          next.id == proxy_id, "next alive wraps");
    check(mewnet_subs_remove_proxy(db, temp_id).code == MEWNET_ERROR_NONE, "remove temp");
    r = mewnet_subs_record_test(db, proxy_id, true, 43, "");
    check(r.error.code == MEWNET_ERROR_NONE && r.rating_delta == 0 && r.rating_after == 1, "rating +1 daily cap");
    r = mewnet_subs_record_test(db, proxy_id, false, 0, "boom");
    check(r.error.code == MEWNET_ERROR_NONE && r.rating_delta == -1 && r.rating_after == 0, "rating -1");
    r = mewnet_subs_record_test(db, proxy_id, false, 0, "boom");
    check(r.error.code == MEWNET_ERROR_NONE && r.rating_delta == 0 && r.rating_after == 0, "rating -1 daily cap");

    err = mewnet_subs_add_proxy(db, group_id,
        "vless://aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee@127.0.0.1:9?encryption=none&security=none&type=tcp#manual");
    check(err.code == MEWNET_ERROR_NONE, "add manual");
    int64_t count = 0;
    err = mewnet_subs_count_proxies(db, group_id, &count);
    check(err.code == MEWNET_ERROR_NONE && count == 2, "count proxies");
    int manual_id = query_int(raw, "SELECT id FROM proxies WHERE name='manual' LIMIT 1");
    mewnet_subs_proxy manual;
    err = mewnet_subs_get_proxy(db, manual_id, &manual);
    check(err.code == MEWNET_ERROR_NONE && manual.manual, "get manual");
    err = mewnet_subs_remove_proxy(db, manual_id);
    check(err.code == MEWNET_ERROR_NONE, "add manual");

    char rating_sql[128];
    snprintf(rating_sql, sizeof(rating_sql), "SELECT rating FROM proxies WHERE id=%d", proxy_id);
    int rating = query_int(raw, rating_sql);
    check(rating == 0, "final rating");
    int events = query_int(raw, "SELECT COUNT(*) FROM test_events");
    check(events == 4, "test events");
    mewnet_subs_remove_proxy(db, proxy_id);

    err = mewnet_subs_remove_subscription(db, sub_id);
    check(err.code == MEWNET_ERROR_NONE, "add manual");
    int proxies = query_int(raw, "SELECT COUNT(*) FROM proxies");
    int events_after = query_int(raw, "SELECT COUNT(*) FROM test_events");
    check(proxies == 0, "proxies cascade");
    check(events_after == 0, "events cascade");

    sqlite3_close(raw);
    mewnet_subs_close(db);
    unlink(path);

    if (failures == 0) {
        printf("rating tests passed\n");
        return 0;
    }
    fprintf(stderr, "rating tests failed\n");
    return 1;
}
