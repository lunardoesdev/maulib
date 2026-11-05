#include "milib_subs_internal.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char* k_schema =
    "PRAGMA foreign_keys = ON;"
    "CREATE TABLE IF NOT EXISTS subscriptions ("
    " id INTEGER PRIMARY KEY,"
    " url TEXT NOT NULL UNIQUE,"
    " name TEXT NOT NULL DEFAULT '',"
    " priority INTEGER NOT NULL DEFAULT 0,"
    " enabled INTEGER NOT NULL DEFAULT 1,"
    " fetch_interval_sec INTEGER NOT NULL DEFAULT 21600,"
    " next_fetch_at INTEGER NOT NULL DEFAULT 0,"
    " last_fetch_at INTEGER,"
    " last_status INTEGER,"
    " failure_count INTEGER NOT NULL DEFAULT 0,"
    " etag TEXT NOT NULL DEFAULT '',"
    " last_modified TEXT NOT NULL DEFAULT '',"
    " userinfo TEXT NOT NULL DEFAULT '',"
    " last_error TEXT NOT NULL DEFAULT '',"
    " generation INTEGER NOT NULL DEFAULT 0,"
    " created_at INTEGER NOT NULL"
    ");"
    "CREATE TABLE IF NOT EXISTS proxies ("
    " id INTEGER PRIMARY KEY,"
    " subscription_id INTEGER NOT NULL REFERENCES subscriptions(id) ON DELETE CASCADE,"
    " link TEXT NOT NULL,"
    " link_hash BLOB NOT NULL,"
    " name TEXT NOT NULL DEFAULT '',"
    " protocol TEXT NOT NULL DEFAULT '',"
    " server TEXT NOT NULL DEFAULT '',"
    " port INTEGER NOT NULL DEFAULT 0,"
    " rating INTEGER NOT NULL DEFAULT 0,"
    " alive INTEGER NOT NULL DEFAULT 0,"
    " last_tested_at INTEGER,"
    " last_delay_ms INTEGER,"
    " last_error TEXT NOT NULL DEFAULT '',"
    " last_up_day INTEGER NOT NULL DEFAULT 0,"
    " last_down_day INTEGER NOT NULL DEFAULT 0,"
    " last_seen_generation INTEGER NOT NULL DEFAULT 0,"
    " UNIQUE(subscription_id, link_hash)"
    ");"
    "CREATE TABLE IF NOT EXISTS test_events ("
    " id INTEGER PRIMARY KEY,"
    " proxy_id INTEGER NOT NULL REFERENCES proxies(id) ON DELETE CASCADE,"
    " ts INTEGER NOT NULL,"
    " ok INTEGER NOT NULL,"
    " delay_ms INTEGER,"
    " error TEXT NOT NULL DEFAULT '',"
    " rating_before INTEGER NOT NULL,"
    " rating_after INTEGER NOT NULL,"
    " rating_delta INTEGER NOT NULL"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_proxies_sub_gen ON proxies(subscription_id, last_seen_generation);"
    "CREATE INDEX IF NOT EXISTS idx_proxies_test ON proxies(last_tested_at);"
    "CREATE INDEX IF NOT EXISTS idx_subs_priority ON subscriptions(priority, next_fetch_at);"
    "PRAGMA user_version = 1;";

milib_subs_config milib_subs_config_default(void) {
    milib_subs_config config;
    memset(&config, 0, sizeof(config));
    config.user_agent = "milib-subs/0.1";
    config.test_url = SUBS_DEFAULT_TEST_URL;
    config.test_expected = SUBS_DEFAULT_TEST_EXPECTED;
    config.test_timeout_ms = SUBS_DEFAULT_TEST_TIMEOUT_MS;
    config.request_limit = 10;
    config.request_window_seconds = 240;
    config.default_fetch_interval_sec = SUBS_DEFAULT_FETCH_INTERVAL;
    return config;
}

static void copy_config_string(char* dst, size_t dst_size, const char* src, const char* fallback) {
    const char* value = (src != NULL && src[0] != '\0') ? src : fallback;
    if (dst_size == 0) {
        return;
    }
    if (value == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t len = strlen(value);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, value, len);
    dst[len] = '\0';
}

milib_subs_open_result milib_subs_open(const milib_subs_config* config) {
    milib_subs_open_result result;
    memset(&result, 0, sizeof(result));

    milib_subs_config defaults = milib_subs_config_default();
    if (config == NULL) {
        config = &defaults;
    }
    if (config->db_path == NULL || config->db_path[0] == '\0') {
        result.error = subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db_path is required");
        return result;
    }

    milib_subs* db = (milib_subs*)calloc(1, sizeof(milib_subs));
    if (db == NULL) {
        result.error = subs_make_error(MILIB_ERROR_FAILED, "out of memory");
        return result;
    }
    copy_config_string(db->user_agent, sizeof(db->user_agent), config->user_agent,
                       defaults.user_agent);
    copy_config_string(db->test_url, sizeof(db->test_url), config->test_url,
                       defaults.test_url);
    copy_config_string(db->test_expected, sizeof(db->test_expected), config->test_expected,
                       defaults.test_expected);
    db->test_timeout_ms = config->test_timeout_ms > 0 ? config->test_timeout_ms
                                                      : defaults.test_timeout_ms;
    db->request_limit = config->request_limit;
    db->request_window_seconds = config->request_window_seconds;
    db->default_fetch_interval_sec = config->default_fetch_interval_sec > 0
                                         ? config->default_fetch_interval_sec
                                         : defaults.default_fetch_interval_sec;
    db->rng = (unsigned int)(subs_now_ms() ^ (int64_t)getpid());
    pthread_mutex_init(&db->rate_lock, NULL);

    int rc = sqlite3_open_v2(config->db_path, &db->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        result.error = subs_db_error(db->db, "open database");
        if (db->db != NULL) {
            sqlite3_close(db->db);
        }
        pthread_mutex_destroy(&db->rate_lock);
        free(db);
        return result;
    }
    sqlite3_busy_timeout(db->db, 5000);
    sqlite3_extended_result_codes(db->db, 1);

    milib_error err;
    subs_clear_error(&err);
    if (subs_db_exec(db->db, "PRAGMA journal_mode = WAL;", &err) != 0 ||
        subs_db_exec(db->db, "PRAGMA synchronous = NORMAL;", &err) != 0 ||
        subs_db_exec(db->db, "PRAGMA foreign_keys = ON;", &err) != 0 ||
        subs_db_exec(db->db, k_schema, &err) != 0) {
        result.error = err;
        sqlite3_close(db->db);
        pthread_mutex_destroy(&db->rate_lock);
        free(db);
        return result;
    }

    result.db = db;
    result.error = subs_make_error(MILIB_ERROR_NONE, "");
    return result;
}

void milib_subs_close(milib_subs* db) {
    if (db == NULL) {
        return;
    }
    if (db->db != NULL) {
        sqlite3_close(db->db);
    }
    pthread_mutex_destroy(&db->rate_lock);
    free(db);
}

milib_error milib_subs_add_subscription(milib_subs* db, const char* url,
                                        int priority, const char* name) {
    if (db == NULL || url == NULL || url[0] == '\0') {
        return subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db and url are required");
    }
    sqlite3_stmt* stmt = NULL;
    milib_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "INSERT INTO subscriptions(url, priority, name, created_at, next_fetch_at)"
                        " VALUES(?,?,?,?,0)"
                        " ON CONFLICT(url) DO UPDATE SET"
                        " priority=excluded.priority,"
                        " name=CASE WHEN excluded.name<>'' THEN excluded.name ELSE subscriptions.name END,"
                        " enabled=1",
                        &err) != 0) {
        return err;
    }
    subs_db_bind_text(stmt, 1, url);
    sqlite3_bind_int(stmt, 2, priority);
    subs_db_bind_text(stmt, 3, name != NULL ? name : "");
    sqlite3_bind_int64(stmt, 4, subs_now_ms());
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "add subscription");
    }
    return subs_make_error(MILIB_ERROR_NONE, "");
}

milib_error milib_subs_remove_subscription(milib_subs* db, int64_t id) {
    if (db == NULL) {
        return subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    milib_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt, "DELETE FROM subscriptions WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "remove subscription");
    }
    if (sqlite3_changes(db->db) == 0) {
        return subs_make_error(MILIB_ERROR_NOT_FOUND, "subscription %lld not found",
                               (long long)id);
    }
    return subs_make_error(MILIB_ERROR_NONE, "");
}

milib_error milib_subs_set_priority(milib_subs* db, int64_t id, int priority) {
    if (db == NULL) {
        return subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    milib_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "UPDATE subscriptions SET priority=? WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int(stmt, 1, priority);
    sqlite3_bind_int64(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "set priority");
    }
    if (sqlite3_changes(db->db) == 0) {
        return subs_make_error(MILIB_ERROR_NOT_FOUND, "subscription %lld not found",
                               (long long)id);
    }
    return subs_make_error(MILIB_ERROR_NONE, "");
}

milib_error milib_subs_set_enabled(milib_subs* db, int64_t id, bool enabled) {
    if (db == NULL) {
        return subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    milib_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "UPDATE subscriptions SET enabled=? WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int(stmt, 1, enabled ? 1 : 0);
    sqlite3_bind_int64(stmt, 2, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "set enabled");
    }
    if (sqlite3_changes(db->db) == 0) {
        return subs_make_error(MILIB_ERROR_NOT_FOUND, "subscription %lld not found",
                               (long long)id);
    }
    return subs_make_error(MILIB_ERROR_NONE, "");
}

milib_subscription_list milib_subs_load_subscriptions(milib_subs* db) {
    milib_subscription_list result;
    memset(&result, 0, sizeof(result));
    if (db == NULL) {
        result.error = subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }

    sqlite3_stmt* count_stmt = NULL;
    milib_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &count_stmt, "SELECT COUNT(*) FROM subscriptions", &err) != 0) {
        result.error = err;
        return result;
    }
    size_t count = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) {
        count = (size_t)sqlite3_column_int64(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);

    milib_subscription* items = NULL;
    if (count > 0) {
        items = (milib_subscription*)calloc(count, sizeof(milib_subscription));
        if (items == NULL) {
            result.error = subs_make_error(MILIB_ERROR_FAILED, "out of memory");
            return result;
        }
    }

    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT id,url,name,priority,enabled,last_fetch_at,next_fetch_at,"
                        "last_status,failure_count,etag,last_modified,userinfo,last_error"
                        " FROM subscriptions ORDER BY priority DESC, id ASC",
                        &err) != 0) {
        free(items);
        result.error = err;
        return result;
    }

    size_t used = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < count) {
        milib_subscription* item = &items[used++];
        item->id = sqlite3_column_int64(stmt, 0);
        subs_db_copy_text(stmt, 1, item->url, sizeof(item->url));
        subs_db_copy_text(stmt, 2, item->name, sizeof(item->name));
        item->priority = sqlite3_column_int(stmt, 3);
        item->enabled = sqlite3_column_int(stmt, 4) != 0;
        item->last_fetch_at = sqlite3_column_int64(stmt, 5);
        item->next_fetch_at = sqlite3_column_int64(stmt, 6);
        item->last_status = sqlite3_column_int(stmt, 7);
        item->failure_count = sqlite3_column_int(stmt, 8);
        subs_db_copy_text(stmt, 9, item->etag, sizeof(item->etag));
        subs_db_copy_text(stmt, 10, item->last_modified, sizeof(item->last_modified));
        subs_db_copy_text(stmt, 11, item->userinfo, sizeof(item->userinfo));
        subs_db_copy_text(stmt, 12, item->last_error, sizeof(item->last_error));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        free(items);
        result.error = subs_db_error(db->db, "load subscriptions");
        return result;
    }

    result.items = items;
    result.count = used;
    result.error = subs_make_error(MILIB_ERROR_NONE, "");
    return result;
}

void milib_subs_unload_subscriptions(milib_subscription_list list) {
    free(list.items);
}

milib_subs_proxy_list milib_subs_load_proxies(milib_subs* db,
                                               int64_t subscription_id,
                                               size_t limit,
                                               size_t offset) {
    milib_subs_proxy_list result;
    memset(&result, 0, sizeof(result));
    if (db == NULL) {
        result.error = subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }
    if (limit == 0) {
        limit = 100;
    }
    if (limit > 10000) {
        limit = 10000;
    }

    milib_error err;
    subs_clear_error(&err);

    const char* count_all = "SELECT COUNT(*) FROM proxies";
    const char* count_one = "SELECT COUNT(*) FROM proxies WHERE subscription_id=?";
    sqlite3_stmt* count_stmt = NULL;
    if (subs_db_prepare(db->db, &count_stmt,
                        subscription_id > 0 ? count_one : count_all, &err) != 0) {
        result.error = err;
        return result;
    }
    if (subscription_id > 0) {
        sqlite3_bind_int64(count_stmt, 1, subscription_id);
    }
    size_t total = 0;
    if (sqlite3_step(count_stmt) == SQLITE_ROW) {
        total = (size_t)sqlite3_column_int64(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);
    if (total > limit) {
        total = limit;
    }

    const char* sql_all =
        "SELECT id,subscription_id,link,name,protocol,server,port,rating,alive,"
        "last_tested_at,last_delay_ms,last_error FROM proxies"
        " ORDER BY id ASC LIMIT ? OFFSET ?";
    const char* sql_one =
        "SELECT id,subscription_id,link,name,protocol,server,port,rating,alive,"
        "last_tested_at,last_delay_ms,last_error FROM proxies"
        " WHERE subscription_id=? ORDER BY id ASC LIMIT ? OFFSET ?";

    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        subscription_id > 0 ? sql_one : sql_all, &err) != 0) {
        result.error = err;
        return result;
    }

    int index = 1;
    if (subscription_id > 0) {
        sqlite3_bind_int64(stmt, index++, subscription_id);
    }
    sqlite3_bind_int64(stmt, index++, (int64_t)limit);
    sqlite3_bind_int64(stmt, index++, (int64_t)offset);

    milib_subs_proxy* items = NULL;
    if (total > 0) {
        items = (milib_subs_proxy*)calloc(total, sizeof(milib_subs_proxy));
        if (items == NULL) {
            sqlite3_finalize(stmt);
            result.error = subs_make_error(MILIB_ERROR_FAILED, "out of memory");
            return result;
        }
    }

    size_t used = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < total) {
        milib_subs_proxy* item = &items[used++];
        item->id = sqlite3_column_int64(stmt, 0);
        item->subscription_id = sqlite3_column_int64(stmt, 1);
        subs_db_copy_text(stmt, 2, item->link, sizeof(item->link));
        subs_db_copy_text(stmt, 3, item->name, sizeof(item->name));
        subs_db_copy_text(stmt, 4, item->protocol, sizeof(item->protocol));
        subs_db_copy_text(stmt, 5, item->server, sizeof(item->server));
        item->port = sqlite3_column_int(stmt, 6);
        item->rating = sqlite3_column_int(stmt, 7);
        item->alive = sqlite3_column_int(stmt, 8) != 0;
        item->last_tested_at = sqlite3_column_int64(stmt, 9);
        item->last_delay_ms = (unsigned)sqlite3_column_int64(stmt, 10);
        subs_db_copy_text(stmt, 11, item->last_error, sizeof(item->last_error));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        free(items);
        result.error = subs_db_error(db->db, "load proxies");
        return result;
    }

    result.items = items;
    result.count = used;
    result.error = subs_make_error(MILIB_ERROR_NONE, "");
    return result;
}

void milib_subs_unload_proxies(milib_subs_proxy_list list) {
    free(list.items);
}

milib_subs_stats milib_subs_get_stats(milib_subs* db) {
    milib_subs_stats stats;
    memset(&stats, 0, sizeof(stats));
    if (db == NULL) {
        return stats;
    }
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db->db,
                           "SELECT"
                           " (SELECT COUNT(*) FROM subscriptions),"
                           " (SELECT COUNT(*) FROM proxies),"
                           " (SELECT COUNT(*) FROM proxies WHERE alive=1),"
                           " (SELECT COUNT(*) FROM proxies WHERE alive=0 AND last_tested_at IS NOT NULL),"
                           " (SELECT COUNT(*) FROM test_events)",
                           -1, &stmt, NULL) != SQLITE_OK) {
        return stats;
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        stats.subscriptions = sqlite3_column_int64(stmt, 0);
        stats.proxies = sqlite3_column_int64(stmt, 1);
        stats.alive = sqlite3_column_int64(stmt, 2);
        stats.dead = sqlite3_column_int64(stmt, 3);
        stats.tests = sqlite3_column_int64(stmt, 4);
    }
    sqlite3_finalize(stmt);
    return stats;
}
