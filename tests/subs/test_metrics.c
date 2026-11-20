/*
 * Latency/speed metrics: every probe stores TTFB (the ping), TTLB, body bytes
 * and throughput; selection ranks on rating + latency + speed, not rating only.
 */

#include "mewnet_subs_internal.h"

#include <sqlite3.h>
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

static int table_has(sqlite3* db, const char* table, const char* column) {
    char sql[128];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    sqlite3_stmt* stmt = NULL;
    int found = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* name = (const char*)sqlite3_column_text(stmt, 1);
        if (name != NULL && strcmp(name, column) == 0) {
            found = 1;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return found;
}

static int64_t add(mewnet_subs* db, int64_t group, const char* link) {
    int64_t id = 0;
    mewnet_error err = mewnet_subs_add_proxy_id(db, group, link, &id);
    if (err.code != MEWNET_ERROR_NONE) {
        fprintf(stderr, "add proxy: %s\n", err.message);
        return 0;
    }
    return id;
}

static void record(mewnet_subs* db, int64_t id, int rating_hint,
                   unsigned ttfb, unsigned ttlb, unsigned long long bytes,
                   unsigned long long speed) {
    (void)rating_hint;
    mewnet_subs_test_result r = mewnet_subs_record_test_metrics(
        db, id, true, ttlb, ttfb, ttlb, bytes, speed, "");
    check(r.error.code == MEWNET_ERROR_NONE, "record metrics");
}

int main(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/mewnet-subs-metrics-%ld.db", (long)getpid());
    unlink(path);

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = path;
    config.request_limit = 0;
    config.test_url = "http://127.0.0.1:9/204";
    config.test_timeout_ms = 300;
    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open: %s\n", opened.error.message);
        return 1;
    }
    mewnet_subs* db = opened.db;

    check(table_has(db->db, "proxies", "last_ttfb_ms"), "schema: proxies.last_ttfb_ms");
    check(table_has(db->db, "proxies", "last_speed_bps"), "schema: proxies.last_speed_bps");
    check(table_has(db->db, "test_events", "ttfb_ms"), "schema: test_events.ttfb_ms");

    int64_t group = 0;
    mewnet_subs_group_list groups = mewnet_subs_load_groups(db);
    if (groups.count > 0) {
        group = groups.items[0].id;
    }
    mewnet_subs_unload_groups(groups);
    check(group > 0, "default group");

    int64_t a = add(db, group, "vless://aaaaaaaa-1111-2222-3333-444444444444@127.0.0.1:11");
    int64_t b = add(db, group, "vless://bbbbbbbb-1111-2222-3333-444444444444@127.0.0.1:12");
    int64_t c = add(db, group, "vless://cccccccc-1111-2222-3333-444444444444@127.0.0.1:13");
    check(a > 0 && b > 0 && c > 0, "add three proxies");

    /* A has the best latency and a good speed; C has the best rating. Early
     * tests give C its rating by recording it five days in a row. */
    record(db, a, 0, 100, 120, 100000, 1000000);
    record(db, b, 0, 500, 520, 100000, 5000000);
    record(db, c, 5, 900, 940, 100000, 100000);

    /* Ratings grow once per day, too slowly for a test; set them directly. */
    {
        char sql[256];
        snprintf(sql, sizeof(sql), "UPDATE proxies SET rating=1 WHERE id IN (%lld,%lld);"
                                   "UPDATE proxies SET rating=5 WHERE id=%lld;",
                 (long long)a, (long long)b, (long long)c);
        check(sqlite3_exec(db->db, sql, NULL, NULL, NULL) == SQLITE_OK, "set ratings");
    }

    mewnet_subs_proxy row;
    check(mewnet_subs_get_proxy(db, a, &row).code == MEWNET_ERROR_NONE, "get proxy");
    check(row.last_delay_ms == 120, "stored delay = TTLB");
    check(row.last_ttfb_ms == 100, "stored TTFB");
    check(row.last_ttlb_ms == 120, "stored TTLB");
    check(row.last_bytes == 100000, "stored bytes");
    check(row.last_speed_bps == 1000000, "stored speed");

    mewnet_subs_proxy best;
    int64_t exclude_before = 0;
    mewnet_error sel = mewnet_subs_random_best_alive(db, group, 0, 0, 1, &best);
    check(sel.code == MEWNET_ERROR_NONE, "select best");
    check(best.id == a, "best = lowest latency with good speed, not highest rating");
    exclude_before = best.id;

    /* Make B the fastest and quickest: it must win even with rating 0. */
    record(db, b, 0, 110, 130, 200000, 5000000);
    sel = mewnet_subs_random_best_alive(db, group, a, 0, 1, &best);
    check(sel.code == MEWNET_ERROR_NONE, "select second best");
    check(best.id == b, "best = fastest after re-measure");

    /* Proxies without a speed (default 204) still select by rating+latency. */
    record(db, a, 0, 50, 60, 0, 0);
    sel = mewnet_subs_random_best_alive(db, group, 0, 0, 1, &best);
    check(sel.code == MEWNET_ERROR_NONE, "select without speed");
    check(best.id == a, "no-speed probe ranks on latency");

    /* Health probes refresh the same metrics. */
    mewnet_error herr = mewnet_subs_record_health_metrics(db, a, true, 95, 85, 95, 4096, 200000);
    check(herr.code == MEWNET_ERROR_NONE, "record health metrics");
    check(mewnet_subs_get_proxy(db, a, &row).code == MEWNET_ERROR_NONE, "get after health");
    check(row.last_delay_ms == 95, "health delay");
    check(row.last_ttfb_ms == 85, "health TTFB");
    check(row.last_ttlb_ms == 95, "health TTLB");
    check(row.last_bytes == 4096, "health bytes");
    check(row.last_speed_bps == 200000, "health speed");

    /* Every test writes its metrics into the event log too. */
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db->db,
                           "SELECT COUNT(*), MIN(ttfb_ms), MAX(speed_bps) FROM test_events",
                           -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        check(sqlite3_column_int(stmt, 0) >= 4, "event per test");
        check(sqlite3_column_int(stmt, 1) > 0, "event TTFB stored");
        check(sqlite3_column_int(stmt, 2) > 0, "event speed stored");
    } else {
        check(0, "select events");
    }
    sqlite3_finalize(stmt);

    mewnet_subs_close(db);
    unlink(path);

    /* An older database (v2, no metric columns) is migrated on open. */
    snprintf(path, sizeof(path), "/tmp/mewnet-subs-metrics-v2-%ld.db", (long)getpid());
    unlink(path);
    sqlite3* raw = NULL;
    if (sqlite3_open(path, &raw) == SQLITE_OK) {
        char* sql =
            "CREATE TABLE subscription_groups(id INTEGER PRIMARY KEY, name TEXT NOT NULL UNIQUE,"
            " priority INTEGER NOT NULL DEFAULT 0, enabled INTEGER NOT NULL DEFAULT 1,"
            " created_at INTEGER NOT NULL);"
            "CREATE TABLE subscriptions(id INTEGER PRIMARY KEY, group_id INTEGER NOT NULL,"
            " url TEXT NOT NULL UNIQUE, name TEXT NOT NULL DEFAULT '',"
            " priority INTEGER NOT NULL DEFAULT 0, enabled INTEGER NOT NULL DEFAULT 1,"
            " fetch_interval_sec INTEGER NOT NULL DEFAULT 21600,"
            " next_fetch_at INTEGER NOT NULL DEFAULT 0, last_fetch_at INTEGER,"
            " last_status INTEGER, failure_count INTEGER NOT NULL DEFAULT 0,"
            " etag TEXT NOT NULL DEFAULT '', last_modified TEXT NOT NULL DEFAULT '',"
            " userinfo TEXT NOT NULL DEFAULT '', last_error TEXT NOT NULL DEFAULT '',"
            " generation INTEGER NOT NULL DEFAULT 0, created_at INTEGER NOT NULL);"
            "CREATE TABLE proxies(id INTEGER PRIMARY KEY, group_id INTEGER NOT NULL,"
            " link TEXT NOT NULL, link_hash BLOB NOT NULL, identity_hash BLOB,"
            " name TEXT NOT NULL DEFAULT '', protocol TEXT NOT NULL DEFAULT '',"
            " server TEXT NOT NULL DEFAULT '', port INTEGER NOT NULL DEFAULT 0,"
            " rating INTEGER NOT NULL DEFAULT 0, health_failures INTEGER NOT NULL DEFAULT 0,"
            " alive INTEGER NOT NULL DEFAULT 0, manual INTEGER NOT NULL DEFAULT 0,"
            " last_tested_at INTEGER, last_delay_ms INTEGER,"
            " last_error TEXT NOT NULL DEFAULT '', last_up_day INTEGER NOT NULL DEFAULT 0,"
            " last_down_day INTEGER NOT NULL DEFAULT 0, UNIQUE(group_id, link_hash));"
            "CREATE TABLE proxy_sources(proxy_id INTEGER NOT NULL, subscription_id INTEGER NOT NULL,"
            " last_seen_generation INTEGER NOT NULL, PRIMARY KEY(proxy_id, subscription_id));"
            "CREATE TABLE test_events(id INTEGER PRIMARY KEY, proxy_id INTEGER NOT NULL,"
            " ts INTEGER NOT NULL, ok INTEGER NOT NULL, delay_ms INTEGER,"
            " error TEXT NOT NULL DEFAULT '', rating_before INTEGER NOT NULL,"
            " rating_after INTEGER NOT NULL, rating_delta INTEGER NOT NULL);"
            "PRAGMA user_version = 2;";
        sqlite3_exec(raw, sql, NULL, NULL, NULL);
        sqlite3_close(raw);
    }
    config.db_path = path;
    opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        fprintf(stderr, "open v2: %s\n", opened.error.message);
        failures++;
    } else {
        check(table_has(opened.db->db, "proxies", "last_ttfb_ms"), "migration adds proxy TTFB");
        check(table_has(opened.db->db, "test_events", "speed_bps"), "migration adds event speed");
        mewnet_subs_close(opened.db);
    }
    unlink(path);

    if (failures == 0) {
        printf("metrics tests passed\n");
    }
    return failures == 0 ? 0 : 1;
}
