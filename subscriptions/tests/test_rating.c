#include "milib_subs_internal.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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
    snprintf(path, sizeof(path), "/tmp/milib-subs-rating-%ld.db", (long)getpid());
    unlink(path);

    milib_subs_config config = milib_subs_config_default();
    config.db_path = path;
    config.request_limit = 0;
    milib_subs_open_result opened = milib_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open: %s\n", opened.error.message);
        return 1;
    }
    milib_subs* db = opened.db;

    milib_error err = milib_subs_add_subscription(db, "https://example.test/sub", 10, "test");
    if (err.code != MILIB_ERROR_NONE) {
        fprintf(stderr, "add: %s\n", err.message);
        return 1;
    }

    sqlite3* raw = NULL;
    if (sqlite3_open(path, &raw) != SQLITE_OK) {
        fprintf(stderr, "raw open failed\n");
        return 1;
    }
    int sub_id = query_int(raw, "SELECT id FROM subscriptions LIMIT 1");
    char insert[512];
    snprintf(insert, sizeof(insert),
             "INSERT INTO proxies(subscription_id,link,link_hash,last_seen_generation)"
             " VALUES(%d,'vless://x',X'00010203',1)", sub_id);
    if (sqlite3_exec(raw, insert, NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "insert proxy failed: %s\n", sqlite3_errmsg(raw));
        return 1;
    }
    int proxy_id = query_int(raw, "SELECT id FROM proxies LIMIT 1");

    int failures = 0;
    milib_subs_test_result r;

    r = milib_subs_record_test(db, proxy_id, true, 42, "");
    failures += (r.error.code != MILIB_ERROR_NONE || r.rating_delta != 1 || r.rating_after != 1);
    r = milib_subs_record_test(db, proxy_id, true, 43, "");
    failures += (r.error.code != MILIB_ERROR_NONE || r.rating_delta != 0 || r.rating_after != 1);
    r = milib_subs_record_test(db, proxy_id, false, 0, "boom");
    failures += (r.error.code != MILIB_ERROR_NONE || r.rating_delta != -1 || r.rating_after != 0);
    r = milib_subs_record_test(db, proxy_id, false, 0, "boom");
    failures += (r.error.code != MILIB_ERROR_NONE || r.rating_delta != 0 || r.rating_after != 0);

    err = milib_subs_add_proxy(db, sub_id,
        "vless://aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee@127.0.0.1:9?encryption=none&security=none&type=tcp#manual");
    failures += (err.code != MILIB_ERROR_NONE);
    int64_t count = 0;
    err = milib_subs_count_proxies(db, sub_id, &count);
    failures += (err.code != MILIB_ERROR_NONE || count != 2);
    int manual_id = query_int(raw, "SELECT id FROM proxies WHERE name='manual' LIMIT 1");
    milib_subs_proxy manual;
    err = milib_subs_get_proxy(db, manual_id, &manual);
    failures += (err.code != MILIB_ERROR_NONE || !manual.manual);
    err = milib_subs_remove_proxy(db, manual_id);
    failures += (err.code != MILIB_ERROR_NONE);

    int rating = query_int(raw, "SELECT rating FROM proxies LIMIT 1");
    int events = query_int(raw, "SELECT COUNT(*) FROM test_events");
    failures += (rating != 0);
    failures += (events != 4);

    err = milib_subs_remove_subscription(db, sub_id);
    failures += (err.code != MILIB_ERROR_NONE);
    int proxies = query_int(raw, "SELECT COUNT(*) FROM proxies");
    int events_after = query_int(raw, "SELECT COUNT(*) FROM test_events");
    failures += (proxies != 0);
    failures += (events_after != 0);

    sqlite3_close(raw);
    milib_subs_close(db);
    unlink(path);

    if (failures == 0) {
        printf("rating tests passed\n");
        return 0;
    }
    fprintf(stderr, "rating tests failed\n");
    return 1;
}
