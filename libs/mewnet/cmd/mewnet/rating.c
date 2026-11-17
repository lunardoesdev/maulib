#include "mewnet_subs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* candidate_filter(mewnet_subs_candidate_scope scope) {
    switch (scope) {
    case MEWNET_SUBS_CANDIDATE_UNTESTED:
        return " AND p.last_tested_at IS NULL";
    case MEWNET_SUBS_CANDIDATE_NOT_ALIVE:
        return " AND p.alive=0";
    case MEWNET_SUBS_CANDIDATE_ANY:
    default:
        return "";
    }
}

static int select_candidate(mewnet_subs* db, mewnet_subs_candidate_scope scope,
                            int64_t* proxy_id, char* link, size_t link_size,
                            mewnet_error* err) {
    char sql[1024];
    snprintf(sql, sizeof(sql),
             "SELECT q.id, q.link FROM ("
             " SELECT p.id AS id, p.link AS link, p.rating AS rating"
             " FROM proxies p"
             " JOIN subscription_groups g ON g.id=p.group_id"
             " WHERE g.enabled=1%s"
             " GROUP BY p.id"
             ") q"
             " ORDER BY pow("
             " ((random() & 9223372036854775807) / 9223372036854775807.0),"
             " 1.0 / (1.0 + max(0, q.rating + 10))"
             " ) DESC LIMIT 1",
             candidate_filter(scope));
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt, sql, err) != 0) {
        return -1;
    }
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            *err = subs_make_error(MEWNET_ERROR_NOT_FOUND, "no proxies to test");
        } else {
            *err = subs_db_error(db->db, "select candidate");
        }
        return -1;
    }
    *proxy_id = sqlite3_column_int64(stmt, 0);
    subs_db_copy_text(stmt, 1, link, link_size);
    sqlite3_finalize(stmt);
    return 0;
}

static int select_proxy_link(mewnet_subs* db, int64_t proxy_id, char* link, size_t link_size,
                             mewnet_error* err) {
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt, "SELECT link FROM proxies WHERE id=?", err) != 0) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, proxy_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            *err = subs_make_error(MEWNET_ERROR_NOT_FOUND, "proxy %lld not found",
                                   (long long)proxy_id);
        } else {
            *err = subs_db_error(db->db, "select proxy");
        }
        return -1;
    }
    subs_db_copy_text(stmt, 0, link, link_size);
    sqlite3_finalize(stmt);
    return 0;
}

mewnet_subs_test_result mewnet_subs_record_test(mewnet_subs* db, int64_t proxy_id,
                                               bool alive, unsigned delay_ms,
                                               const char* error_text) {
    mewnet_subs_test_result result;
    memset(&result, 0, sizeof(result));
    result.proxy_id = proxy_id;
    result.alive = alive;
    result.delay_ms = delay_ms;
    if (error_text != NULL) {
        snprintf(result.test_error, sizeof(result.test_error), "%s", error_text);
    }

    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }

    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_exec(db->db, "BEGIN IMMEDIATE;", &err) != 0) {
        result.error = err;
        return result;
    }

    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT rating,last_up_day,last_down_day FROM proxies WHERE id=?",
                        &err) != 0) {
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }
    sqlite3_bind_int64(stmt, 1, proxy_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = (rc == SQLITE_DONE)
                           ? subs_make_error(MEWNET_ERROR_NOT_FOUND, "proxy %lld not found",
                                             (long long)proxy_id)
                           : subs_db_error(db->db, "select proxy rating");
        return result;
    }
    int rating = sqlite3_column_int(stmt, 0);
    int last_up_day = sqlite3_column_int(stmt, 1);
    int last_down_day = sqlite3_column_int(stmt, 2);
    sqlite3_finalize(stmt);

    int day = subs_today_utc();
    int delta = 0;
    if (alive) {
        if (last_up_day != day) {
            delta = 1;
            last_up_day = day;
        }
    } else {
        if (last_down_day != day) {
            delta = -1;
            last_down_day = day;
        }
    }
    int new_rating = rating + delta;
    int64_t now = subs_now_ms();

    if (subs_db_prepare(db->db, &stmt,
                        "UPDATE proxies SET rating=?, last_up_day=?, last_down_day=?,"
                        " alive=?, last_tested_at=?, last_delay_ms=?, last_error=?,"
                        " name=CASE WHEN ?<>'' THEN ? ELSE name END,"
                        " protocol=CASE WHEN ?<>'' THEN ? ELSE protocol END,"
                        " server=CASE WHEN ?<>'' THEN ? ELSE server END,"
                        " port=CASE WHEN ?>0 THEN ? ELSE port END"
                        " WHERE id=?",
                        &err) != 0) {
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }
    sqlite3_bind_int(stmt, 1, new_rating);
    sqlite3_bind_int(stmt, 2, last_up_day);
    sqlite3_bind_int(stmt, 3, last_down_day);
    sqlite3_bind_int(stmt, 4, alive ? 1 : 0);
    sqlite3_bind_int64(stmt, 5, now);
    sqlite3_bind_int64(stmt, 6, (int64_t)delay_ms);
    subs_db_bind_text(stmt, 7, alive ? "" : result.test_error);
    subs_db_bind_text(stmt, 8, result.name);
    subs_db_bind_text(stmt, 9, result.name);
    subs_db_bind_text(stmt, 10, result.protocol);
    subs_db_bind_text(stmt, 11, result.protocol);
    subs_db_bind_text(stmt, 12, result.server);
    subs_db_bind_text(stmt, 13, result.server);
    sqlite3_bind_int(stmt, 14, result.port);
    sqlite3_bind_int(stmt, 15, result.port);
    sqlite3_bind_int64(stmt, 16, proxy_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = subs_db_error(db->db, "update proxy rating");
        return result;
    }

    if (subs_db_prepare(db->db, &stmt,
                        "INSERT INTO test_events(proxy_id,ts,ok,delay_ms,error,"
                        "rating_before,rating_after,rating_delta) VALUES(?,?,?,?,?,?,?,?)",
                        &err) != 0) {
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }
    sqlite3_bind_int64(stmt, 1, proxy_id);
    sqlite3_bind_int64(stmt, 2, now);
    sqlite3_bind_int(stmt, 3, alive ? 1 : 0);
    sqlite3_bind_int64(stmt, 4, (int64_t)delay_ms);
    subs_db_bind_text(stmt, 5, alive ? "" : result.test_error);
    sqlite3_bind_int(stmt, 6, rating);
    sqlite3_bind_int(stmt, 7, new_rating);
    sqlite3_bind_int(stmt, 8, delta);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = subs_db_error(db->db, "insert test event");
        return result;
    }

    if (subs_db_exec(db->db, "COMMIT;", &err) != 0) {
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }

    result.rating_before = rating;
    result.rating_after = new_rating;
    result.rating_delta = delta;
    result.error = subs_make_error(MEWNET_ERROR_NONE, "");
    return result;
}

static void update_proxy_meta(mewnet_subs* db, int64_t proxy_id,
                              const mewnet_link_test_result* tested) {
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "UPDATE proxies SET name=CASE WHEN ?<>'' THEN ? ELSE name END,"
                        " protocol=CASE WHEN ?<>'' THEN ? ELSE protocol END,"
                        " server=CASE WHEN ?<>'' THEN ? ELSE server END,"
                        " port=CASE WHEN ?>0 THEN ? ELSE port END WHERE id=?",
                        &err) != 0) {
        return;
    }
    subs_db_bind_text(stmt, 1, tested->name);
    subs_db_bind_text(stmt, 2, tested->name);
    subs_db_bind_text(stmt, 3, tested->proxy_type);
    subs_db_bind_text(stmt, 4, tested->proxy_type);
    subs_db_bind_text(stmt, 5, tested->server);
    subs_db_bind_text(stmt, 6, tested->server);
    sqlite3_bind_int(stmt, 7, tested->port);
    sqlite3_bind_int(stmt, 8, tested->port);
    sqlite3_bind_int64(stmt, 9, proxy_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static mewnet_subs_test_result test_link(mewnet_subs* db, mewnet_instance* mewnet,
                                        int64_t proxy_id, const char* link) {
    mewnet_subs_test_result result;
    memset(&result, 0, sizeof(result));
    result.proxy_id = proxy_id;

    if (mewnet == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "mewnet instance is required");
        return result;
    }

    mewnet_delay_options options = mewnet_delay_options_default();
    options.url = db->test_url;
    options.timeout_ms = db->test_timeout_ms;
    options.expected = db->test_expected;

    mewnet_link_test_result tested = mewnet_test_link_info(mewnet, link, &options);
    if (!tested.ok) {
        result = mewnet_subs_record_test(db, proxy_id, false, 0, tested.error.message);
        result.test_error[sizeof(result.test_error) - 1] = '\0';
        return result;
    }

    result = mewnet_subs_record_test(db, proxy_id, true, tested.delay_ms, "");
    update_proxy_meta(db, proxy_id, &tested);
    snprintf(result.name, sizeof(result.name), "%s", tested.name);
    snprintf(result.protocol, sizeof(result.protocol), "%s", tested.proxy_type);
    snprintf(result.server, sizeof(result.server), "%s", tested.server);
    result.port = tested.port;
    result.delay_ms = tested.delay_ms;
    result.alive = true;
    return result;
}

mewnet_subs_test_result mewnet_subs_test_proxy(mewnet_subs* db, mewnet_instance* mewnet,
                                             int64_t proxy_id) {
    mewnet_subs_test_result result;
    memset(&result, 0, sizeof(result));
    result.proxy_id = proxy_id;
    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }

    result.error = mewnet_subs_rate_limit_wait(db);
    if (result.error.code != MEWNET_ERROR_NONE) {
        return result;
    }

    char link[MEWNET_LINK_MAX];
    mewnet_error err;
    subs_clear_error(&err);
    if (select_proxy_link(db, proxy_id, link, sizeof(link), &err) != 0) {
        result.error = err;
        return result;
    }
    return test_link(db, mewnet, proxy_id, link);
}

static mewnet_subs_test_result test_next_impl(mewnet_subs* db, mewnet_instance* mewnet,
                                              bool wait_for_rate_limit,
                                              mewnet_subs_candidate_scope scope) {
    mewnet_subs_test_result result;
    memset(&result, 0, sizeof(result));
    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }

    if (wait_for_rate_limit) {
        result.error = mewnet_subs_rate_limit_wait(db);
        if (result.error.code != MEWNET_ERROR_NONE) {
            return result;
        }
    }

    int64_t proxy_id = 0;
    char link[MEWNET_LINK_MAX];
    mewnet_error err;
    subs_clear_error(&err);
    if (select_candidate(db, scope, &proxy_id, link, sizeof(link), &err) != 0) {
        result.error = err;
        return result;
    }
    return test_link(db, mewnet, proxy_id, link);
}

mewnet_subs_test_result mewnet_subs_test_next(mewnet_subs* db, mewnet_instance* mewnet) {
    return test_next_impl(db, mewnet, true, MEWNET_SUBS_CANDIDATE_ANY);
}

mewnet_subs_test_result mewnet_subs_test_next_now(mewnet_subs* db, mewnet_instance* mewnet) {
    return test_next_impl(db, mewnet, false, MEWNET_SUBS_CANDIDATE_ANY);
}

mewnet_subs_test_result mewnet_subs_test_candidate_now(mewnet_subs* db, mewnet_instance* mewnet,
                                                       mewnet_subs_candidate_scope scope) {
    return test_next_impl(db, mewnet, false, scope);
}

mewnet_error mewnet_subs_record_health(mewnet_subs* db, int64_t proxy_id, bool ok) {
    return mewnet_subs_record_health_delay(db, proxy_id, ok, 0);
}

mewnet_error mewnet_subs_record_health_delay(mewnet_subs* db, int64_t proxy_id, bool ok,
                                             unsigned delay_ms) {
    if (db == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "UPDATE proxies SET health_failures=CASE WHEN ? THEN 0"
                        " ELSE health_failures+1 END,"
                        " last_delay_ms=CASE WHEN ? THEN ? ELSE last_delay_ms END"
                        " WHERE id=?", &err) != 0) {
        return err;
    }
    sqlite3_bind_int(stmt, 1, ok ? 1 : 0);
    sqlite3_bind_int(stmt, 2, (ok && delay_ms > 0) ? 1 : 0);
    sqlite3_bind_int64(stmt, 3, (int64_t)delay_ms);
    sqlite3_bind_int64(stmt, 4, proxy_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return subs_db_error(db->db, "record proxy health");
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}
