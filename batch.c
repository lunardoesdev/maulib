#include "mewnet_subs_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* dup_string(const char* value) {
    if (value == NULL) {
        return NULL;
    }
    size_t len = strlen(value) + 1;
    char* copy = (char*)malloc(len);
    if (copy != NULL) {
        memcpy(copy, value, len);
    }
    return copy;
}

mewnet_subs_batch_test_result mewnet_subs_test_batch(mewnet_subs* db, mewnet_instance* milib,
                                                      int64_t group_id, size_t limit,
                                                      size_t concurrency) {
    mewnet_subs_batch_test_result result;
    memset(&result, 0, sizeof(result));
    if (db == NULL || milib == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT,
                                       "db and milib instance are required");
        return result;
    }
    if (limit == 0) {
        limit = 70;
    }
    if (limit > 1000) {
        limit = 1000;
    }
    if (concurrency == 0) {
        concurrency = limit;
    }
    if (concurrency > 256) {
        concurrency = 256;
    }

    mewnet_error err;
    subs_clear_error(&err);
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT q.id, q.link FROM ("
                        " SELECT p.id AS id, p.link AS link, p.rating AS rating,"
                        " p.last_tested_at AS last_tested_at"
                        " FROM proxies p"
                        " WHERE (?=0 OR p.group_id=?) AND (p.last_tested_at IS NULL OR p.alive=0)"
                        " GROUP BY p.id"
                        ") q"
                        " ORDER BY CASE WHEN q.last_tested_at IS NULL THEN 0 ELSE 1 END,"
                        " pow("
                        " ((random() & 9223372036854775807) / 9223372036854775807.0),"
                        " 1.0 / (1.0 + max(0, q.rating + 10))"
                        " ) DESC LIMIT ?",
                        &err) != 0) {
        result.error = err;
        return result;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, group_id);
    sqlite3_bind_int64(stmt, 3, (int64_t)limit);

    size_t capacity = 0;
    size_t count = 0;
    int64_t* ids = NULL;
    char** links = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count == capacity) {
            size_t next = capacity == 0 ? 16 : capacity * 2;
            if (next > limit) next = limit;
            int64_t* new_ids = (int64_t*)realloc(ids, next * sizeof(int64_t));
            if (new_ids == NULL) {
                sqlite3_finalize(stmt);
                for (size_t i = 0; i < count; i++) free(links[i]);
                free(links);
                free(ids);
                result.error = subs_make_error(MEWNET_ERROR_FAILED, "out of memory");
                return result;
            }
            ids = new_ids;
            char** new_links = (char**)realloc(links, next * sizeof(char*));
            if (new_links == NULL) {
                sqlite3_finalize(stmt);
                for (size_t i = 0; i < count; i++) free(links[i]);
                free(links);
                free(ids);
                result.error = subs_make_error(MEWNET_ERROR_FAILED, "out of memory");
                return result;
            }
            links = new_links;
            capacity = next;
        }
        ids[count] = sqlite3_column_int64(stmt, 0);
        links[count] = dup_string(subs_db_column_text(stmt, 1));
        if (links[count] == NULL) {
            sqlite3_finalize(stmt);
            for (size_t i = 0; i < count; i++) free(links[i]);
            free(links);
            free(ids);
            result.error = subs_make_error(MEWNET_ERROR_FAILED, "out of memory");
            return result;
        }
        count++;
    }
    sqlite3_finalize(stmt);

    if (count == 0) {
        free(links);
        free(ids);
        result.error = subs_make_error(MEWNET_ERROR_NONE, "");
        return result;
    }

    mewnet_delay_options options = mewnet_delay_options_default();
    options.url = db->test_url;
    options.timeout_ms = db->test_timeout_ms;
    options.expected = db->test_expected;

    mewnet_batch_test_result tested = mewnet_test_links_batch_first(
        milib, (const char* const*)links, count, &options, concurrency);
    if (!tested.ok) {
        result.error = tested.error;
        for (size_t i = 0; i < count; i++) free(links[i]);
        free(links);
        free(ids);
        return result;
    }

    for (size_t i = 0; i < count && i < tested.count; i++) {
        mewnet_link_test_result* item = &tested.items[i];
        if (item->error.code == MEWNET_ERROR_CANCELLED) {
            continue;
        }
        mewnet_subs_test_result record = mewnet_subs_record_test_metrics(
            db, ids[i], item->ok, item->delay_ms,
            item->ttfb_ms, item->ttlb_ms, item->bytes, item->speed_bps,
            item->ok ? "" : item->error.message);
        if (record.error.code != MEWNET_ERROR_NONE && result.error.code == MEWNET_ERROR_NONE) {
            result.error = record.error;
        }
        result.tested++;
        if (item->ok) {
            result.alive++;
            if (result.first_alive_proxy_id == 0) {
                result.first_alive_proxy_id = ids[i];
                snprintf(result.first_alive_link, sizeof(result.first_alive_link), "%s", links[i]);
                snprintf(result.first_alive_name, sizeof(result.first_alive_name), "%s",
                         item->name[0] != '\0' ? item->name : links[i]);
            }
        }
    }

    mewnet_unload_batch_test(tested);
    for (size_t i = 0; i < count; i++) free(links[i]);
    free(links);
    free(ids);
    if (result.error.code == MEWNET_ERROR_NONE) {
        subs_clear_error(&result.error);
    }
    return result;
}

mewnet_error mewnet_subs_count_untested(mewnet_subs* db, int64_t group_id,
                                        int64_t* out_count) {
    if (db == NULL || out_count == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and out_count are required");
    }
    *out_count = 0;
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    const char* sql_all = "SELECT COUNT(*) FROM proxies WHERE last_tested_at IS NULL";
    const char* sql_group = "SELECT COUNT(*) FROM proxies WHERE group_id=? AND last_tested_at IS NULL";
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

mewnet_error mewnet_subs_count_alive(mewnet_subs* db, int64_t group_id,
                                     int64_t* out_count) {
    if (db == NULL || out_count == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and out_count are required");
    }
    *out_count = 0;
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    const char* sql_all = "SELECT COUNT(*) FROM proxies WHERE alive=1";
    const char* sql_group = "SELECT COUNT(*) FROM proxies WHERE group_id=? AND alive=1";
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

mewnet_error mewnet_subs_first_alive(mewnet_subs* db, int64_t group_id,
                                     mewnet_subs_proxy* out) {
    if (db == NULL || out == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and out are required");
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    const char* sql =
        "SELECT id FROM proxies WHERE alive=1 AND (?=0 OR group_id=?)"
        " ORDER BY health_failures ASC, rating DESC,"
        " CASE WHEN COALESCE(NULLIF(last_ttfb_ms,0), NULLIF(last_delay_ms,0)) IS NULL"
        "      THEN 2147483647"
        "      ELSE COALESCE(NULLIF(last_ttfb_ms,0), last_delay_ms) END ASC,"
        " COALESCE(last_speed_bps,0) DESC,"
        " last_tested_at DESC, id ASC LIMIT 1";
    if (subs_db_prepare(db->db, &stmt, sql, &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, group_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            return subs_make_error(MEWNET_ERROR_NOT_FOUND, "no alive proxy found");
        }
        return subs_db_error(db->db, "select first alive proxy");
    }
    int64_t id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return mewnet_subs_get_proxy(db, id, out);
}

mewnet_error mewnet_subs_random_best_alive(mewnet_subs* db, int64_t group_id,
                                           int64_t exclude_id, int64_t avoid_id,
                                           size_t candidates, mewnet_subs_proxy* out) {
    if (db == NULL || out == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and out are required");
    }
    memset(out, 0, sizeof(*out));
    if (candidates == 0) {
        candidates = 5;
    }
    if (candidates > 50) {
        candidates = 50;
    }

    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    /* Weighted choice, not "rating only": rating, latency (TTFB, falling back
     * to the old delay) and throughput (TTLB-based, when the test URL returned
     * a body) are normalised against the pool. The top `candidates` window is
     * then sampled randomly so equal-quality nodes rotate. */
    const char* sql =
        "WITH pool AS ("
        " SELECT id, health_failures, rating,"
        "  COALESCE(NULLIF(last_ttfb_ms,0), NULLIF(last_delay_ms,0)) AS latency,"
        "  NULLIF(last_speed_bps,0) AS speed"
        " FROM proxies"
        " WHERE alive=1 AND (?=0 OR group_id=?) AND id<>? AND id<>?"
        "), stats AS ("
        " SELECT MIN(latency) AS min_latency, MAX(speed) AS max_speed FROM pool"
        "), scored AS ("
        " SELECT pool.id AS id, pool.health_failures AS failures,"
        "  (0.40 * CASE"
        "     WHEN pool.rating <= -20 THEN 0.0"
        "     WHEN pool.rating >= 20 THEN 1.0"
        "     ELSE (pool.rating + 20) / 40.0 END"
        "   + 0.40 * CASE"
        "     WHEN pool.latency IS NULL OR stats.min_latency IS NULL THEN 0.0"
        "     ELSE stats.min_latency * 1.0 / pool.latency END"
        "   + 0.20 * CASE"
        "     WHEN pool.speed IS NULL OR stats.max_speed IS NULL THEN 0.5"
        "     ELSE pool.speed * 1.0 / stats.max_speed END) AS score"
        " FROM pool, stats"
        ")"
        " SELECT id FROM ("
        "  SELECT id FROM scored ORDER BY failures ASC, score DESC, id ASC LIMIT ?"
        " ) ORDER BY random() LIMIT 1";
    if (subs_db_prepare(db->db, &stmt, sql, &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, group_id);
    sqlite3_bind_int64(stmt, 3, exclude_id);
    sqlite3_bind_int64(stmt, 4, avoid_id);
    sqlite3_bind_int64(stmt, 5, (int64_t)candidates);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            return subs_make_error(MEWNET_ERROR_NOT_FOUND, "no alive candidates found");
        }
        return subs_db_error(db->db, "select random best alive proxy");
    }
    int64_t id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return mewnet_subs_get_proxy(db, id, out);
}

mewnet_error mewnet_subs_next_alive(mewnet_subs* db, int64_t group_id,
                                    int64_t exclude_id, mewnet_subs_proxy* out) {
    if (db == NULL || out == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db and out are required");
    }
    memset(out, 0, sizeof(*out));
    if (exclude_id == 0) {
        return mewnet_subs_first_alive(db, group_id, out);
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    const char* sql =
        "SELECT id FROM proxies"
        " WHERE alive=1 AND (?=0 OR group_id=?) AND id<>?"
        " ORDER BY CASE WHEN id>? THEN 0 ELSE 1 END, id ASC LIMIT 1";
    if (subs_db_prepare(db->db, &stmt, sql, &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, group_id);
    sqlite3_bind_int64(stmt, 3, exclude_id);
    sqlite3_bind_int64(stmt, 4, exclude_id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            return subs_make_error(MEWNET_ERROR_NOT_FOUND, "no other alive proxy found");
        }
        return subs_db_error(db->db, "select next alive proxy");
    }
    int64_t id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return mewnet_subs_get_proxy(db, id, out);
}

mewnet_error mewnet_subs_median_delay(mewnet_subs* db, int64_t group_id,
                                      int64_t exclude_id, size_t candidates,
                                      unsigned* median_ms, size_t* samples) {
    if (median_ms != NULL) *median_ms = 0;
    if (samples != NULL) *samples = 0;
    if (db == NULL) {
        return subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
    }
    if (candidates == 0) {
        candidates = 5;
    }
    if (candidates > 50) {
        candidates = 50;
    }

    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    const char* sql =
        "SELECT t.delay FROM ("
        " SELECT last_delay_ms AS delay FROM proxies"
        " WHERE alive=1 AND (?=0 OR group_id=?) AND id<>?"
        " AND last_delay_ms IS NOT NULL AND last_delay_ms>0"
        " ORDER BY health_failures ASC, rating DESC, last_delay_ms ASC, id ASC"
        " LIMIT ?"
        ") t ORDER BY t.delay ASC";
    if (subs_db_prepare(db->db, &stmt, sql, &err) != 0) {
        return err;
    }
    sqlite3_bind_int64(stmt, 1, group_id);
    sqlite3_bind_int64(stmt, 2, group_id);
    sqlite3_bind_int64(stmt, 3, exclude_id);
    sqlite3_bind_int64(stmt, 4, (int64_t)candidates);

    unsigned values[50];
    size_t count = 0;
    int rc = sqlite3_step(stmt);
    while (rc == SQLITE_ROW && count < sizeof(values) / sizeof(values[0])) {
        unsigned delay = (unsigned)sqlite3_column_int64(stmt, 0);
        if (delay > 0) {
            values[count++] = delay;
        }
        rc = sqlite3_step(stmt);
    }
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return subs_db_error(db->db, "select pool delays");
    }
    sqlite3_finalize(stmt);

    if (samples != NULL) *samples = count;
    if (count == 0) {
        return subs_make_error(MEWNET_ERROR_NOT_FOUND, "no measured delays in the pool");
    }
    /* Rows come back sorted, so the median sits in the middle. */
    if (median_ms != NULL) {
        if (count % 2 == 1) {
            *median_ms = values[count / 2];
        } else {
            *median_ms = (values[count / 2 - 1] + values[count / 2]) / 2;
        }
    }
    return subs_make_error(MEWNET_ERROR_NONE, "");
}
