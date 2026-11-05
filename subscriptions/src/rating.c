#include "milib_subs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int select_candidate(milib_subs* db, int64_t* proxy_id, char* link, size_t link_size,
                            milib_error* err) {
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT p.id, p.link FROM proxies p"
                        " JOIN subscriptions s ON s.id=p.subscription_id"
                        " WHERE s.enabled=1"
                        " ORDER BY s.priority DESC, p.rating DESC,"
                        " COALESCE(p.last_tested_at,0) ASC, p.id ASC LIMIT 1",
                        err) != 0) {
        return -1;
    }
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            *err = subs_make_error(MILIB_ERROR_NOT_FOUND, "no proxies to test");
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

static int select_proxy_link(milib_subs* db, int64_t proxy_id, char* link, size_t link_size,
                             milib_error* err) {
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt, "SELECT link FROM proxies WHERE id=?", err) != 0) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, proxy_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            *err = subs_make_error(MILIB_ERROR_NOT_FOUND, "proxy %lld not found",
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

milib_subs_test_result milib_subs_record_test(milib_subs* db, int64_t proxy_id,
                                               bool alive, unsigned delay_ms,
                                               const char* error_text) {
    milib_subs_test_result result;
    memset(&result, 0, sizeof(result));
    result.proxy_id = proxy_id;
    result.alive = alive;
    result.delay_ms = delay_ms;
    if (error_text != NULL) {
        snprintf(result.test_error, sizeof(result.test_error), "%s", error_text);
    }

    if (db == NULL) {
        result.error = subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }

    milib_error err;
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
                           ? subs_make_error(MILIB_ERROR_NOT_FOUND, "proxy %lld not found",
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
    result.error = subs_make_error(MILIB_ERROR_NONE, "");
    return result;
}

static void update_proxy_meta(milib_subs* db, int64_t proxy_id,
                              const milib_link_test_result* tested) {
    sqlite3_stmt* stmt = NULL;
    milib_error err;
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

static milib_subs_test_result test_link(milib_subs* db, milib_instance* milib,
                                        int64_t proxy_id, const char* link) {
    milib_subs_test_result result;
    memset(&result, 0, sizeof(result));
    result.proxy_id = proxy_id;

    if (milib == NULL) {
        result.error = subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "milib instance is required");
        return result;
    }

    milib_delay_options options = milib_delay_options_default();
    options.url = db->test_url;
    options.timeout_ms = db->test_timeout_ms;
    options.expected = db->test_expected;

    milib_link_test_result tested = milib_test_link_info(milib, link, &options);
    if (!tested.ok) {
        result = milib_subs_record_test(db, proxy_id, false, 0, tested.error.message);
        result.test_error[sizeof(result.test_error) - 1] = '\0';
        return result;
    }

    result = milib_subs_record_test(db, proxy_id, true, tested.delay_ms, "");
    update_proxy_meta(db, proxy_id, &tested);
    snprintf(result.name, sizeof(result.name), "%s", tested.name);
    snprintf(result.protocol, sizeof(result.protocol), "%s", tested.proxy_type);
    snprintf(result.server, sizeof(result.server), "%s", tested.server);
    result.port = tested.port;
    result.delay_ms = tested.delay_ms;
    result.alive = true;
    return result;
}

milib_subs_test_result milib_subs_test_proxy(milib_subs* db, milib_instance* milib,
                                             int64_t proxy_id) {
    milib_subs_test_result result;
    memset(&result, 0, sizeof(result));
    result.proxy_id = proxy_id;
    if (db == NULL) {
        result.error = subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }

    result.error = milib_subs_rate_limit_wait(db);
    if (result.error.code != MILIB_ERROR_NONE) {
        return result;
    }

    char link[MILIB_LINK_MAX];
    milib_error err;
    subs_clear_error(&err);
    if (select_proxy_link(db, proxy_id, link, sizeof(link), &err) != 0) {
        result.error = err;
        return result;
    }
    return test_link(db, milib, proxy_id, link);
}

milib_subs_test_result milib_subs_test_next(milib_subs* db, milib_instance* milib) {
    milib_subs_test_result result;
    memset(&result, 0, sizeof(result));
    if (db == NULL) {
        result.error = subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }

    result.error = milib_subs_rate_limit_wait(db);
    if (result.error.code != MILIB_ERROR_NONE) {
        return result;
    }

    int64_t proxy_id = 0;
    char link[MILIB_LINK_MAX];
    milib_error err;
    subs_clear_error(&err);
    if (select_candidate(db, &proxy_id, link, sizeof(link), &err) != 0) {
        result.error = err;
        return result;
    }
    return test_link(db, milib, proxy_id, link);
}
