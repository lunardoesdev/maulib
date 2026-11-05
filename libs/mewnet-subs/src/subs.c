#include "mewnet_subs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static const char* k_groups_schema =
    "CREATE TABLE IF NOT EXISTS subscription_groups ("
    " id INTEGER PRIMARY KEY,"
    " name TEXT NOT NULL UNIQUE,"
    " priority INTEGER NOT NULL DEFAULT 0,"
    " enabled INTEGER NOT NULL DEFAULT 1,"
    " created_at INTEGER NOT NULL"
    ");";

static const char* k_subscriptions_schema =
    "CREATE TABLE IF NOT EXISTS subscriptions ("
    " id INTEGER PRIMARY KEY,"
    " group_id INTEGER NOT NULL REFERENCES subscription_groups(id) ON DELETE CASCADE,"
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
    ");";

static const char* k_proxies_schema =
    "CREATE TABLE IF NOT EXISTS proxies ("
    " id INTEGER PRIMARY KEY,"
    " group_id INTEGER NOT NULL REFERENCES subscription_groups(id) ON DELETE CASCADE,"
    " link TEXT NOT NULL,"
    " link_hash BLOB NOT NULL,"
    " name TEXT NOT NULL DEFAULT '',"
    " protocol TEXT NOT NULL DEFAULT '',"
    " server TEXT NOT NULL DEFAULT '',"
    " port INTEGER NOT NULL DEFAULT 0,"
    " rating INTEGER NOT NULL DEFAULT 0,"
    " alive INTEGER NOT NULL DEFAULT 0,"
    " manual INTEGER NOT NULL DEFAULT 0,"
    " last_tested_at INTEGER,"
    " last_delay_ms INTEGER,"
    " last_error TEXT NOT NULL DEFAULT '',"
    " last_up_day INTEGER NOT NULL DEFAULT 0,"
    " last_down_day INTEGER NOT NULL DEFAULT 0,"
    " UNIQUE(group_id, link_hash)"
    ");";

static const char* k_sources_schema =
    "CREATE TABLE IF NOT EXISTS proxy_sources ("
    " proxy_id INTEGER NOT NULL REFERENCES proxies(id) ON DELETE CASCADE,"
    " subscription_id INTEGER NOT NULL REFERENCES subscriptions(id) ON DELETE CASCADE,"
    " last_seen_generation INTEGER NOT NULL,"
    " PRIMARY KEY(proxy_id, subscription_id)"
    ");";

static const char* k_events_schema =
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
    ");";

static const char* k_indexes_schema =
    "CREATE INDEX IF NOT EXISTS idx_proxies_group ON proxies(group_id);"
    "CREATE INDEX IF NOT EXISTS idx_proxies_test ON proxies(last_tested_at);"
    "CREATE INDEX IF NOT EXISTS idx_subs_group ON subscriptions(group_id, priority);"
    "CREATE INDEX IF NOT EXISTS idx_subs_priority ON subscriptions(priority, next_fetch_at);"
    "CREATE INDEX IF NOT EXISTS idx_sources_sub ON proxy_sources(subscription_id, last_seen_generation);"
    "CREATE INDEX IF NOT EXISTS idx_events_proxy ON test_events(proxy_id, ts);";

mewnet_subs_config mewnet_subs_config_default(void) {
    mewnet_subs_config config;
    memset(&config, 0, sizeof(config));
    config.user_agent = "mewnet-subs/0.2";
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

static int table_has_column(sqlite3* db, const char* table, const char* column) {
    char sql[128];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s)", table);
    sqlite3_stmt* stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return 0;
    }
    int found = 0;
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

static int subs_create_schema(sqlite3* db, mewnet_error* err) {
    return subs_db_exec(db, k_groups_schema, err) == 0 &&
                   subs_db_exec(db, k_subscriptions_schema, err) == 0 &&
                   subs_db_exec(db, k_proxies_schema, err) == 0 &&
                   subs_db_exec(db, k_sources_schema, err) == 0 &&
                   subs_db_exec(db, k_events_schema, err) == 0
               ? 0
               : -1;
}

static int64_t subs_default_group_id(mewnet_subs* db, mewnet_error* err) {
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT id FROM subscription_groups ORDER BY id LIMIT 1", err) != 0) {
        return 0;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        *err = subs_db_error(db->db, "select default group");
        return 0;
    }

    if (subs_db_prepare(db->db, &stmt,
                        "INSERT INTO subscription_groups(name,priority,enabled,created_at)"
                        " VALUES('All',0,1,?)",
                        err) != 0) {
        return 0;
    }
    sqlite3_bind_int64(stmt, 1, subs_now_ms());
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        *err = subs_db_error(db->db, "create default group");
        return 0;
    }
    return sqlite3_last_insert_rowid(db->db);
}

static int subs_migrate(mewnet_subs* db, int64_t default_group, mewnet_error* err) {
    /* v1 did not have manual. */
    if (!table_has_column(db->db, "proxies", "manual")) {
        if (subs_db_exec(db->db,
                         "ALTER TABLE proxies ADD COLUMN manual INTEGER NOT NULL DEFAULT 0;",
                         err) != 0) {
            return -1;
        }
    }

    if (!table_has_column(db->db, "subscriptions", "group_id")) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "ALTER TABLE subscriptions ADD COLUMN group_id INTEGER NOT NULL DEFAULT %lld;",
                 (long long)default_group);
        if (subs_db_exec(db->db, sql, err) != 0) {
            return -1;
        }
        snprintf(sql, sizeof(sql),
                 "UPDATE subscriptions SET group_id=%lld WHERE group_id IS NULL OR group_id=0;",
                 (long long)default_group);
        if (subs_db_exec(db->db, sql, err) != 0) {
            return -1;
        }
    }

    if (!table_has_column(db->db, "proxies", "group_id")) {
        if (subs_db_exec(db->db,
                         "DROP TABLE IF EXISTS proxy_sources;"
                         "DROP INDEX IF EXISTS idx_proxies_sub_gen;"
                         "DROP INDEX IF EXISTS idx_proxies_test;"
                         "DROP INDEX IF EXISTS idx_proxies_group;"
                         "ALTER TABLE proxies RENAME TO proxies_v1;",
                         err) != 0) {
            return -1;
        }
        if (subs_db_exec(db->db, k_proxies_schema, err) != 0 ||
            subs_db_exec(db->db, k_sources_schema, err) != 0 ||
            subs_db_exec(db->db, k_events_schema, err) != 0) {
            return -1;
        }
        if (subs_db_exec(db->db,
                         "INSERT INTO proxies(group_id,link,link_hash,name,protocol,server,port,"
                         "rating,alive,manual,last_tested_at,last_delay_ms,last_error,"
                         "last_up_day,last_down_day)"
                         " SELECT s.group_id, po.link, po.link_hash,"
                         " MAX(po.name), MAX(po.protocol), MAX(po.server), MAX(po.port),"
                         " MAX(po.rating), MAX(po.alive), MAX(po.manual),"
                         " MAX(po.last_tested_at), MAX(po.last_delay_ms), MAX(po.last_error),"
                         " MAX(po.last_up_day), MAX(po.last_down_day)"
                         " FROM proxies_v1 po JOIN subscriptions s ON s.id=po.subscription_id"
                         " GROUP BY s.group_id, po.link_hash;",
                         err) != 0) {
            return -1;
        }
        if (subs_db_exec(db->db,
                         "INSERT OR IGNORE INTO proxy_sources(proxy_id,subscription_id,last_seen_generation)"
                         " SELECT p.id, po.subscription_id, MAX(po.last_seen_generation)"
                         " FROM proxies_v1 po"
                         " JOIN subscriptions s ON s.id=po.subscription_id"
                         " JOIN proxies p ON p.group_id=s.group_id AND p.link_hash=po.link_hash"
                         " GROUP BY p.id, po.subscription_id;",
                         err) != 0) {
            return -1;
        }
        if (subs_db_exec(db->db, "DROP TABLE proxies_v1;", err) != 0) {
            return -1;
        }
    }

    if (subs_db_exec(db->db, k_indexes_schema, err) != 0) {
        return -1;
    }

    char version[64];
    snprintf(version, sizeof(version), "PRAGMA user_version = 2;");
    return subs_db_exec(db->db, version, err);
}

static int subs_delete_orphans(mewnet_subs* db, int64_t group_id, size_t* removed, mewnet_error* err) {
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        "DELETE FROM proxies WHERE group_id=? AND manual=0"
                        " AND NOT EXISTS (SELECT 1 FROM proxy_sources ps WHERE ps.proxy_id=proxies.id)",
                        err) != 0) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        *err = subs_db_error(db->db, "delete orphan proxies");
        return -1;
    }
    if (removed != NULL) {
        *removed = (size_t)sqlite3_changes(db->db);
    }
    return 0;
}

mewnet_subs_open_result mewnet_subs_open(const mewnet_subs_config* config) {
    mewnet_subs_open_result result;
    memset(&result, 0, sizeof(result));

    mewnet_subs_config defaults = mewnet_subs_config_default();
    if (config == NULL) {
        config = &defaults;
    }
    if (config->db_path == NULL || config->db_path[0] == '\0') {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db_path is required");
        return result;
    }

    mewnet_subs* db = (mewnet_subs*)calloc(1, sizeof(mewnet_subs));
    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_FAILED, "out of memory");
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
#ifdef _WIN32
    db->rng = (unsigned int)(subs_now_ms() ^ (int64_t)GetCurrentProcessId());
#else
    db->rng = (unsigned int)(subs_now_ms() ^ (int64_t)getpid());
#endif
    subs_mutex_init(&db->rate_lock);

    int rc = sqlite3_open_v2(config->db_path, &db->db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                 SQLITE_OPEN_FULLMUTEX,
                             NULL);
    if (rc != SQLITE_OK) {
        result.error = subs_db_error(db->db, "open database");
        if (db->db != NULL) {
            sqlite3_close(db->db);
        }
        subs_mutex_destroy(&db->rate_lock);
        free(db);
        return result;
    }
    sqlite3_busy_timeout(db->db, 5000);
    sqlite3_extended_result_codes(db->db, 1);

    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_exec(db->db, "PRAGMA journal_mode = WAL;", &err) != 0 ||
        subs_db_exec(db->db, "PRAGMA synchronous = NORMAL;", &err) != 0 ||
        subs_db_exec(db->db, "PRAGMA foreign_keys = ON;", &err) != 0 ||
        subs_create_schema(db->db, &err) != 0) {
        result.error = err;
        sqlite3_close(db->db);
        subs_mutex_destroy(&db->rate_lock);
        free(db);
        return result;
    }

    int64_t default_group = subs_default_group_id(db, &err);
    if (default_group == 0 || subs_migrate(db, default_group, &err) != 0) {
        result.error = err;
        sqlite3_close(db->db);
        subs_mutex_destroy(&db->rate_lock);
        free(db);
        return result;
    }

    result.db = db;
    result.error = subs_make_error(MEWNET_ERROR_NONE, "");
    return result;
}

void mewnet_subs_close(mewnet_subs* db) {
    if (db == NULL) {
        return;
    }
    if (db->db != NULL) {
        sqlite3_close(db->db);
    }
    subs_mutex_destroy(&db->rate_lock);
    free(db);
}

/* Groups. */
mewnet_subs_group_list mewnet_subs_load_groups(mewnet_subs* db) {
    mewnet_subs_group_list result;
    memset(&result, 0, sizeof(result));
    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT id,name,priority,enabled,created_at"
                        " FROM subscription_groups ORDER BY priority DESC, id ASC",
                        &err) != 0) {
        result.error = err;
        return result;
    }
    size_t count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        count++;
    }
    sqlite3_finalize(stmt);
    mewnet_subs_group* items = NULL;
    if (count > 0) {
        items = (mewnet_subs_group*)calloc(count, sizeof(mewnet_subs_group));
        if (items == NULL) {
            result.error = subs_make_error(MEWNET_ERROR_FAILED, "out of memory");
            return result;
        }
    }
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT id,name,priority,enabled,created_at"
                        " FROM subscription_groups ORDER BY priority DESC, id ASC",
                        &err) != 0) {
        free(items);
        result.error = err;
        return result;
    }
    size_t used = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && used < count) {
        mewnet_subs_group* item = &items[used++];
        item->id = sqlite3_column_int64(stmt, 0);
        subs_db_copy_text(stmt, 1, item->name, sizeof(item->name));
        item->priority = sqlite3_column_int(stmt, 2);
        item->enabled = sqlite3_column_int(stmt, 3) != 0;
        item->created_at = sqlite3_column_int64(stmt, 4);
    }
    sqlite3_finalize(stmt);
    result.items = items;
    result.count = used;
    result.error = subs_make_error(MEWNET_ERROR_NONE, "");
    return result;
}

void mewnet_subs_unload_groups(mewnet_subs_group_list list) {
    free(list.items);
}

mewnet_error mewnet_subs_add_group(mewnet_subs* db, const char* name, int priority) {
    if (db == NULL || name == NULL || name[0] == '\0') {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and name are required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "INSERT INTO subscription_groups(name,priority,enabled,created_at)"
                        " VALUES(?,?,1,?)"
                        " ON CONFLICT(name) DO UPDATE SET priority=excluded.priority, enabled=1",
                        &err) != 0) {
        return err;
    }
    subs_db_bind_text(stmt, 1, name);
    sqlite3_bind_int(stmt, 2, priority);
    sqlite3_bind_int64(stmt, 3, subs_now_ms());
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "add group");
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_remove_group(mewnet_subs* db, int64_t group_id) {
    if (db == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt, "SELECT COUNT(*) FROM subscription_groups", &err) != 0) {
        return err;
    }
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (count <= 1) {
        return subs_make_error(MEWNET_ERROR_FAILED, "cannot remove the last group");
    }
    if (subs_db_prepare(db->db, &stmt, "DELETE FROM subscription_groups WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "remove group");
    }
    if (sqlite3_changes(db->db) == 0) {
        return subs_make_error(MEWNET_ERROR_NOT_FOUND, "group %lld not found", (long long)group_id);
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_set_group_priority(mewnet_subs* db, int64_t group_id, int priority) {
    if (db == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "UPDATE subscription_groups SET priority=? WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int(stmt, 1, priority);
    sqlite3_bind_int64(stmt, 2, group_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "set group priority");
    }
    if (sqlite3_changes(db->db) == 0) {
        return subs_make_error(MEWNET_ERROR_NOT_FOUND, "group %lld not found", (long long)group_id);
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_default_group(mewnet_subs* db, int64_t* out_group_id) {
    if (db == NULL || out_group_id == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and out_group_id are required");
    }
    mewnet_error err;
    subs_clear_error(&err);
    int64_t id = subs_default_group_id(db, &err);
    if (id == 0) {
        return err.code != MEWNET_ERROR_NONE ? err
                                             : subs_make_error(MEWNET_ERROR_FAILED, "default group");
    }
    *out_group_id = id;
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_set_subscription_group(mewnet_subs* db, int64_t subscription_id,
                                                int64_t group_id) {
    if (db == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT COUNT(*) FROM proxy_sources WHERE subscription_id=?",
                        &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, subscription_id);
    int sources = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        sources = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (sources > 0) {
        return subs_make_error(MEWNET_ERROR_FAILED,
                               "move subscription before fetching its configs");
    }
    if (subs_db_prepare(db->db, &stmt,
                        "UPDATE subscriptions SET group_id=? WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, subscription_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "set subscription group");
    }
    if (sqlite3_changes(db->db) == 0) {
        return subs_make_error(MEWNET_ERROR_NOT_FOUND, "subscription %lld not found",
                               (long long)subscription_id);
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

/* Subscriptions. */
mewnet_error mewnet_subs_add_subscription_in_group(mewnet_subs* db, int64_t group_id,
                                                   const char* url, int priority,
                                                   const char* name) {
    if (db == NULL || url == NULL || url[0] == '\0') {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and url are required");
    }
    if (group_id == 0) {
        mewnet_error err;
        subs_clear_error(&err);
        group_id = subs_default_group_id(db, &err);
        if (group_id == 0) {
            return err;
        }
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "INSERT INTO subscriptions(group_id,url,priority,name,created_at,next_fetch_at)"
                        " VALUES(?,?,?,?,?,0)"
                        " ON CONFLICT(url) DO UPDATE SET"
                        " group_id=excluded.group_id,"
                        " priority=excluded.priority,"
                        " name=CASE WHEN excluded.name<>'' THEN excluded.name ELSE subscriptions.name END,"
                        " enabled=1",
                        &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    subs_db_bind_text(stmt, 2, url);
    sqlite3_bind_int(stmt, 3, priority);
    subs_db_bind_text(stmt, 4, name != NULL ? name : "");
    sqlite3_bind_int64(stmt, 5, subs_now_ms());
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "add subscription");
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_add_subscription(mewnet_subs* db, const char* url,
                                          int priority, const char* name) {
    return mewnet_subs_add_subscription_in_group(db, 0, url, priority, name);
}

mewnet_error mewnet_subs_remove_subscription(mewnet_subs* db, int64_t id) {
    if (db == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt, "SELECT group_id FROM subscriptions WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return rc == SQLITE_DONE
                   ? subs_make_error(MEWNET_ERROR_NOT_FOUND, "subscription %lld not found",
                                     (long long)id)
                   : subs_db_error(db->db, "find subscription");
    }
    int64_t group_id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);

    if (subs_db_prepare(db->db, &stmt, "DELETE FROM subscriptions WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "remove subscription");
    }
    subs_delete_orphans(db, group_id, NULL, &err);
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_set_priority(mewnet_subs* db, int64_t id, int priority) {
    if (db == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
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
        return subs_make_error(MEWNET_ERROR_NOT_FOUND, "subscription %lld not found",
                               (long long)id);
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_set_enabled(mewnet_subs* db, int64_t id, bool enabled) {
    if (db == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
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
        return subs_make_error(MEWNET_ERROR_NOT_FOUND, "subscription %lld not found",
                               (long long)id);
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

static mewnet_subscription_list load_subscriptions(mewnet_subs* db, int64_t group_id, bool filtered) {
    mewnet_subscription_list result;
    memset(&result, 0, sizeof(result));
    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }
    mewnet_error err;
    subs_clear_error(&err);

    const char* count_all =
        "SELECT COUNT(*) FROM subscriptions s";
    const char* count_group =
        "SELECT COUNT(*) FROM subscriptions s WHERE s.group_id=?";
    const char* select_all =
        "SELECT s.id,s.group_id,s.url,s.name,s.priority,s.enabled,s.last_fetch_at,"
        "s.next_fetch_at,s.last_status,s.failure_count,s.etag,s.last_modified,s.userinfo,s.last_error"
        " FROM subscriptions s JOIN subscription_groups g ON g.id=s.group_id"
        " ORDER BY g.priority DESC, s.priority DESC, s.id ASC";
    const char* select_group =
        "SELECT s.id,s.group_id,s.url,s.name,s.priority,s.enabled,s.last_fetch_at,"
        "s.next_fetch_at,s.last_status,s.failure_count,s.etag,s.last_modified,s.userinfo,s.last_error"
        " FROM subscriptions s WHERE s.group_id=?"
        " ORDER BY s.priority DESC, s.id ASC";

    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt, filtered ? count_group : count_all, &err) != 0) {
        result.error = err;
        return result;
    }
    if (filtered) {
        sqlite3_bind_int64(stmt, 1, group_id);
    }
    size_t count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        count = (size_t)sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    mewnet_subscription* items = NULL;
    if (count > 0) {
        items = (mewnet_subscription*)calloc(count, sizeof(mewnet_subscription));
        if (items == NULL) {
            result.error = subs_make_error(MEWNET_ERROR_FAILED, "out of memory");
            return result;
        }
    }
    if (subs_db_prepare(db->db, &stmt, filtered ? select_group : select_all, &err) != 0) {
        free(items);
        result.error = err;
        return result;
    }
    if (filtered) {
        sqlite3_bind_int64(stmt, 1, group_id);
    }
    size_t used = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < count) {
        mewnet_subscription* item = &items[used++];
        item->id = sqlite3_column_int64(stmt, 0);
        item->group_id = sqlite3_column_int64(stmt, 1);
        subs_db_copy_text(stmt, 2, item->url, sizeof(item->url));
        subs_db_copy_text(stmt, 3, item->name, sizeof(item->name));
        item->priority = sqlite3_column_int(stmt, 4);
        item->enabled = sqlite3_column_int(stmt, 5) != 0;
        item->last_fetch_at = sqlite3_column_int64(stmt, 6);
        item->next_fetch_at = sqlite3_column_int64(stmt, 7);
        item->last_status = sqlite3_column_int(stmt, 8);
        item->failure_count = sqlite3_column_int(stmt, 9);
        subs_db_copy_text(stmt, 10, item->etag, sizeof(item->etag));
        subs_db_copy_text(stmt, 11, item->last_modified, sizeof(item->last_modified));
        subs_db_copy_text(stmt, 12, item->userinfo, sizeof(item->userinfo));
        subs_db_copy_text(stmt, 13, item->last_error, sizeof(item->last_error));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        free(items);
        result.error = subs_db_error(db->db, "load subscriptions");
        return result;
    }
    result.items = items;
    result.count = used;
    result.error = subs_make_error(MEWNET_ERROR_NONE, "");
    return result;
}

mewnet_subscription_list mewnet_subs_load_subscriptions(mewnet_subs* db) {
    return load_subscriptions(db, 0, false);
}

mewnet_subscription_list mewnet_subs_load_subscriptions_in_group(mewnet_subs* db, int64_t group_id) {
    return load_subscriptions(db, group_id, true);
}

void mewnet_subs_unload_subscriptions(mewnet_subscription_list list) {
    free(list.items);
}

/* Proxies. */
static mewnet_subs_proxy_list load_proxies_impl(mewnet_subs* db, int64_t group_id,
                                                size_t limit, size_t offset) {
    mewnet_subs_proxy_list result;
    memset(&result, 0, sizeof(result));
    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }
    if (limit == 0) {
        limit = 100;
    }
    if (limit > 1000) {
        limit = 1000;
    }
    mewnet_error err;
    subs_clear_error(&err);

    const char* count_all = "SELECT COUNT(*) FROM proxies";
    const char* count_group = "SELECT COUNT(*) FROM proxies WHERE group_id=?";
    sqlite3_stmt* count_stmt = NULL;
    if (subs_db_prepare(db->db, &count_stmt, group_id > 0 ? count_group : count_all, &err) != 0) {
        result.error = err;
        return result;
    }
    if (group_id > 0) {
        sqlite3_bind_int64(count_stmt, 1, group_id);
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
        "SELECT id,group_id,link,name,protocol,server,port,rating,alive,manual,"
        "last_tested_at,last_delay_ms,last_error FROM proxies"
        " ORDER BY id ASC LIMIT ? OFFSET ?";
    const char* sql_group =
        "SELECT id,group_id,link,name,protocol,server,port,rating,alive,manual,"
        "last_tested_at,last_delay_ms,last_error FROM proxies"
        " WHERE group_id=? ORDER BY id ASC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt, group_id > 0 ? sql_group : sql_all, &err) != 0) {
        result.error = err;
        return result;
    }
    int index = 1;
    if (group_id > 0) {
        sqlite3_bind_int64(stmt, index++, group_id);
    }
    sqlite3_bind_int64(stmt, index++, (int64_t)limit);
    sqlite3_bind_int64(stmt, index++, (int64_t)offset);

    mewnet_subs_proxy* items = NULL;
    if (total > 0) {
        items = (mewnet_subs_proxy*)calloc(total, sizeof(mewnet_subs_proxy));
        if (items == NULL) {
            sqlite3_finalize(stmt);
            result.error = subs_make_error(MEWNET_ERROR_FAILED, "out of memory");
            return result;
        }
    }
    size_t used = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < total) {
        mewnet_subs_proxy* item = &items[used++];
        item->id = sqlite3_column_int64(stmt, 0);
        item->group_id = sqlite3_column_int64(stmt, 1);
        subs_db_copy_text(stmt, 2, item->link, sizeof(item->link));
        subs_db_copy_text(stmt, 3, item->name, sizeof(item->name));
        subs_db_copy_text(stmt, 4, item->protocol, sizeof(item->protocol));
        subs_db_copy_text(stmt, 5, item->server, sizeof(item->server));
        item->port = sqlite3_column_int(stmt, 6);
        item->rating = sqlite3_column_int(stmt, 7);
        item->alive = sqlite3_column_int(stmt, 8) != 0;
        item->manual = sqlite3_column_int(stmt, 9) != 0;
        item->last_tested_at = sqlite3_column_int64(stmt, 10);
        item->last_delay_ms = (unsigned)sqlite3_column_int64(stmt, 11);
        subs_db_copy_text(stmt, 12, item->last_error, sizeof(item->last_error));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        free(items);
        result.error = subs_db_error(db->db, "load proxies");
        return result;
    }
    result.items = items;
    result.count = used;
    result.error = subs_make_error(MEWNET_ERROR_NONE, "");
    return result;
}

mewnet_subs_proxy_list mewnet_subs_load_proxies(mewnet_subs* db, int64_t group_id,
                                                size_t limit, size_t offset) {
    return load_proxies_impl(db, group_id, limit, offset);
}

void mewnet_subs_unload_proxies(mewnet_subs_proxy_list list) {
    free(list.items);
}

mewnet_subs_proxy_summary_list mewnet_subs_load_proxy_summaries(mewnet_subs* db,
                                                               int64_t group_id,
                                                               size_t limit,
                                                               size_t offset) {
    mewnet_subs_proxy_summary_list result;
    memset(&result, 0, sizeof(result));
    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }
    if (limit == 0) {
        limit = 50;
    }
    if (limit > 1000) {
        limit = 1000;
    }
    mewnet_error err;
    subs_clear_error(&err);
    const char* count_all = "SELECT COUNT(*) FROM proxies";
    const char* count_group = "SELECT COUNT(*) FROM proxies WHERE group_id=?";
    sqlite3_stmt* count_stmt = NULL;
    if (subs_db_prepare(db->db, &count_stmt, group_id > 0 ? count_group : count_all, &err) != 0) {
        result.error = err;
        return result;
    }
    if (group_id > 0) {
        sqlite3_bind_int64(count_stmt, 1, group_id);
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
        "SELECT id,group_id,name,protocol,server,port,rating,alive,manual,"
        "last_tested_at,last_delay_ms,last_error FROM proxies"
        " ORDER BY id ASC LIMIT ? OFFSET ?";
    const char* sql_group =
        "SELECT id,group_id,name,protocol,server,port,rating,alive,manual,"
        "last_tested_at,last_delay_ms,last_error FROM proxies"
        " WHERE group_id=? ORDER BY id ASC LIMIT ? OFFSET ?";
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt, group_id > 0 ? sql_group : sql_all, &err) != 0) {
        result.error = err;
        return result;
    }
    int index = 1;
    if (group_id > 0) {
        sqlite3_bind_int64(stmt, index++, group_id);
    }
    sqlite3_bind_int64(stmt, index++, (int64_t)limit);
    sqlite3_bind_int64(stmt, index++, (int64_t)offset);

    mewnet_subs_proxy_summary* items = NULL;
    if (total > 0) {
        items = (mewnet_subs_proxy_summary*)calloc(total, sizeof(mewnet_subs_proxy_summary));
        if (items == NULL) {
            sqlite3_finalize(stmt);
            result.error = subs_make_error(MEWNET_ERROR_FAILED, "out of memory");
            return result;
        }
    }
    size_t used = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW && used < total) {
        mewnet_subs_proxy_summary* item = &items[used++];
        item->id = sqlite3_column_int64(stmt, 0);
        item->group_id = sqlite3_column_int64(stmt, 1);
        subs_db_copy_text(stmt, 2, item->name, sizeof(item->name));
        subs_db_copy_text(stmt, 3, item->protocol, sizeof(item->protocol));
        subs_db_copy_text(stmt, 4, item->server, sizeof(item->server));
        item->port = sqlite3_column_int(stmt, 5);
        item->rating = sqlite3_column_int(stmt, 6);
        item->alive = sqlite3_column_int(stmt, 7) != 0;
        item->manual = sqlite3_column_int(stmt, 8) != 0;
        item->last_tested_at = sqlite3_column_int64(stmt, 9);
        item->last_delay_ms = (unsigned)sqlite3_column_int64(stmt, 10);
        subs_db_copy_text(stmt, 11, item->last_error, sizeof(item->last_error));
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        free(items);
        result.error = subs_db_error(db->db, "load proxy summaries");
        return result;
    }
    result.items = items;
    result.count = used;
    result.error = subs_make_error(MEWNET_ERROR_NONE, "");
    return result;
}

void mewnet_subs_unload_proxy_summaries(mewnet_subs_proxy_summary_list list) {
    free(list.items);
}

mewnet_error mewnet_subs_get_proxy(mewnet_subs* db, int64_t proxy_id,
                                   mewnet_subs_proxy* out) {
    if (db == NULL || out == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and out are required");
    }
    memset(out, 0, sizeof(*out));
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT id,group_id,link,name,protocol,server,port,"
                        "rating,alive,manual,last_tested_at,last_delay_ms,last_error"
                        " FROM proxies WHERE id=?",
                        &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, proxy_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            return subs_make_error(MEWNET_ERROR_NOT_FOUND, "proxy %lld not found",
                                   (long long)proxy_id);
        }
        return subs_db_error(db->db, "get proxy");
    }
    out->id = sqlite3_column_int64(stmt, 0);
    out->group_id = sqlite3_column_int64(stmt, 1);
    subs_db_copy_text(stmt, 2, out->link, sizeof(out->link));
    subs_db_copy_text(stmt, 3, out->name, sizeof(out->name));
    subs_db_copy_text(stmt, 4, out->protocol, sizeof(out->protocol));
    subs_db_copy_text(stmt, 5, out->server, sizeof(out->server));
    out->port = sqlite3_column_int(stmt, 6);
    out->rating = sqlite3_column_int(stmt, 7);
    out->alive = sqlite3_column_int(stmt, 8) != 0;
    out->manual = sqlite3_column_int(stmt, 9) != 0;
    out->last_tested_at = sqlite3_column_int64(stmt, 10);
    out->last_delay_ms = (unsigned)sqlite3_column_int64(stmt, 11);
    subs_db_copy_text(stmt, 12, out->last_error, sizeof(out->last_error));
    sqlite3_finalize(stmt);
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_add_proxy(mewnet_subs* db, int64_t group_id,
                                   const char* link) {
    if (db == NULL || link == NULL || link[0] == '\0') {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and link are required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt, "SELECT 1 FROM subscription_groups WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_ROW) {
        return subs_make_error(MEWNET_ERROR_NOT_FOUND, "group %lld not found",
                               (long long)group_id);
    }

    uint8_t hash[32];
    subs_link_hash(link, hash);
    char name[MEWNET_NAME_MAX];
    char protocol[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port = 0;
    subs_link_hint(link, name, sizeof(name), protocol, sizeof(protocol),
                   server, sizeof(server), &port);

    if (subs_db_prepare(db->db, &stmt,
                        "INSERT INTO proxies(group_id,link,link_hash,name,protocol,server,port,manual)"
                        " VALUES(?,?,?,?,?,?,?,1)"
                        " ON CONFLICT(group_id,link_hash) DO UPDATE SET"
                        " link=excluded.link,"
                        " name=CASE WHEN excluded.name<>'' THEN excluded.name ELSE proxies.name END,"
                        " protocol=CASE WHEN excluded.protocol<>'' THEN excluded.protocol ELSE proxies.protocol END,"
                        " server=CASE WHEN excluded.server<>'' THEN excluded.server ELSE proxies.server END,"
                        " port=CASE WHEN excluded.port>0 THEN excluded.port ELSE proxies.port END,"
                        " manual=1",
                        &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    subs_db_bind_text(stmt, 2, link);
    subs_db_bind_blob(stmt, 3, hash, (int)sizeof(hash));
    subs_db_bind_text(stmt, 4, name);
    subs_db_bind_text(stmt, 5, protocol);
    subs_db_bind_text(stmt, 6, server);
    sqlite3_bind_int(stmt, 7, port);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "add proxy");
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_remove_proxy(mewnet_subs* db, int64_t proxy_id) {
    if (db == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt, "DELETE FROM proxies WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, proxy_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "remove proxy");
    }
    if (sqlite3_changes(db->db) == 0) {
        return subs_make_error(MEWNET_ERROR_NOT_FOUND, "proxy %lld not found",
                               (long long)proxy_id);
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_error mewnet_subs_count_proxies(mewnet_subs* db, int64_t group_id,
                                       int64_t* out_count) {
    if (db == NULL || out_count == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and out_count are required");
    }
    *out_count = 0;
    const char* sql_all = "SELECT COUNT(*) FROM proxies";
    const char* sql_group = "SELECT COUNT(*) FROM proxies WHERE group_id=?";
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt, group_id > 0 ? sql_group : sql_all, &err) != 0) {
        return err;
    }
    if (group_id > 0) {
        sqlite3_bind_int64(stmt, 1, group_id);
    }
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out_count = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return subs_make_error(MEWNET_ERROR_NONE, "");
}

mewnet_subs_stats mewnet_subs_get_stats(mewnet_subs* db) {
    mewnet_subs_stats stats;
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
