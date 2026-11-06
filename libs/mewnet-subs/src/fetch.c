#include "mewnet_subs_internal.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct sub_row {
    int64_t id;
    int64_t group_id;
    char url[MEWNET_URL_MAX];
    char etag[256];
    char last_modified[256];
    int64_t generation;
    int failure_count;
    int fetch_interval_sec;
} sub_row;

typedef struct ingest_ctx {
    mewnet_subs* subs;
    sqlite3_stmt* insert_proxy;
    sqlite3_stmt* select_proxy;
    sqlite3_stmt* update_proxy;
    sqlite3_stmt* upsert_source;
    int64_t subscription_id;
    int64_t group_id;
    int64_t generation;
    int64_t now;
    int fetch_interval_sec;
    char name[MEWNET_NAME_MAX];
    char userinfo[512];
    mewnet_error error;
    int failed;

    int mode; /* -1 unknown, 0 plain, 1 base64 */
    uint8_t sniff[SUBS_SNIFF_MAX];
    size_t sniff_len;
    subs_b64 b64;
    char line[SUBS_LINE_MAX];
    size_t line_len;
    int discard_line;

    size_t added;
    size_t updated;
    size_t skipped;
} ingest_ctx;

static int ingest_bytes(ingest_ctx* ctx, const uint8_t* data, size_t len);

static int ingest_fail(ingest_ctx* ctx, const char* format, ...) {
    ctx->failed = 1;
    ctx->error.code = MEWNET_ERROR_FAILED;
    va_list args;
    va_start(args, format);
    vsnprintf(ctx->error.message, sizeof(ctx->error.message), format, args);
    va_end(args);
    return -1;
}

static int is_base64_char(uint8_t c) {
    return isalnum(c) || c == '+' || c == '/' || c == '-' || c == '_' || c == '=';
}

static int detect_mode(const uint8_t* data, size_t len) {
    size_t i = 0;
    if (len >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF) {
        i = 3;
    }
    while (i < len && isspace(data[i])) {
        i++;
    }
    if (i >= len) {
        return -1;
    }
    if (data[i] == '#') {
        return 0;
    }
    if (data[i] == '{' || data[i] == '[') {
        return 2;
    }
    size_t scan_end = i + 64 < len ? i + 64 : len;
    for (size_t p = i; p + 2 < scan_end; p++) {
        if (data[p] == ':' && data[p + 1] == '/' && data[p + 2] == '/') {
            return 0;
        }
    }
    int base64 = 1;
    for (size_t p = i; p < len; p++) {
        if (isspace(data[p])) {
            continue;
        }
        if (!is_base64_char(data[p])) {
            base64 = 0;
            break;
        }
    }
    return base64 ? 1 : 0;
}

static int process_link(ingest_ctx* ctx, const char* line) {
    uint8_t hash[32];
    subs_link_hash(line, hash);

    char name[MEWNET_NAME_MAX];
    char protocol[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port = 0;
    subs_link_hint(line, name, sizeof(name), protocol, sizeof(protocol),
                   server, sizeof(server), &port);

    sqlite3_stmt* stmt = ctx->insert_proxy;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, ctx->group_id);
    subs_db_bind_text(stmt, 2, line);
    subs_db_bind_blob(stmt, 3, hash, (int)sizeof(hash));
    subs_db_bind_text(stmt, 4, name);
    subs_db_bind_text(stmt, 5, protocol);
    subs_db_bind_text(stmt, 6, server);
    sqlite3_bind_int(stmt, 7, port);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return ingest_fail(ctx, "insert proxy: %s", sqlite3_errmsg(ctx->subs->db));
    }
    int inserted = sqlite3_changes(ctx->subs->db) > 0;

    stmt = ctx->select_proxy;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, ctx->group_id);
    subs_db_bind_blob(stmt, 2, hash, (int)sizeof(hash));
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_reset(stmt);
        return ingest_fail(ctx, "find proxy: %s", sqlite3_errmsg(ctx->subs->db));
    }
    int64_t proxy_id = sqlite3_column_int64(stmt, 0);
    sqlite3_reset(stmt);

    stmt = ctx->update_proxy;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    subs_db_bind_text(stmt, 1, name);
    subs_db_bind_text(stmt, 2, protocol);
    subs_db_bind_text(stmt, 3, server);
    sqlite3_bind_int(stmt, 4, port);
    sqlite3_bind_int64(stmt, 5, proxy_id);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return ingest_fail(ctx, "update proxy: %s", sqlite3_errmsg(ctx->subs->db));
    }

    stmt = ctx->upsert_source;
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int64(stmt, 1, proxy_id);
    sqlite3_bind_int64(stmt, 2, ctx->subscription_id);
    sqlite3_bind_int64(stmt, 3, ctx->generation);
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        return ingest_fail(ctx, "upsert proxy source: %s", sqlite3_errmsg(ctx->subs->db));
    }
    if (inserted) {
        ctx->added++;
    } else {
        ctx->updated++;
    }
    return 0;
}

static void process_metadata(ingest_ctx* ctx, const char* line) {
    const char* value = NULL;
    if (strncmp(line, "#profile-title:", 15) == 0) {
        value = line + 15;
        while (*value == ' ' || *value == '\t') value++;
        if (strncmp(value, "base64:", 7) == 0) {
            if (subs_b64_decode_string(value + 7, ctx->name, sizeof(ctx->name)) != 0) {
                ctx->name[0] = '\0';
            }
        } else {
            snprintf(ctx->name, sizeof(ctx->name), "%s", value);
        }
        return;
    }
    if (strncmp(line, "#profile-update-interval:", 25) == 0) {
        value = line + 25;
        int hours = atoi(value);
        if (hours > 0) {
            int seconds = hours * 3600;
            if (seconds < SUBS_MIN_FETCH_INTERVAL) seconds = SUBS_MIN_FETCH_INTERVAL;
            if (seconds > SUBS_MAX_FETCH_INTERVAL) seconds = SUBS_MAX_FETCH_INTERVAL;
            ctx->fetch_interval_sec = seconds;
        }
        return;
    }
    if (strncmp(line, "#subscription-userinfo:", 23) == 0) {
        value = line + 23;
        while (*value == ' ' || *value == '\t') value++;
        snprintf(ctx->userinfo, sizeof(ctx->userinfo), "%s", value);
    }
}

static int process_line(ingest_ctx* ctx, char* line) {
    line = subs_trim(line);
    if (line[0] == '\0') {
        return 0;
    }
    if (line[0] == '#') {
        process_metadata(ctx, line);
        return 0;
    }
    if (strstr(line, "://") == NULL) {
        ctx->skipped++;
        return 0;
    }
    return process_link(ctx, line);
}

static int ingest_bytes(ingest_ctx* ctx, const uint8_t* data, size_t len) {
    if (ctx->failed) {
        return -1;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '\n' || c == '\r') {
            if (!ctx->discard_line) {
                ctx->line[ctx->line_len] = '\0';
                if (process_line(ctx, ctx->line) != 0) {
                    return -1;
                }
            }
            ctx->line_len = 0;
            ctx->discard_line = 0;
            continue;
        }
        if (ctx->discard_line) {
            continue;
        }
        if (ctx->line_len + 1 >= sizeof(ctx->line)) {
            ctx->discard_line = 1;
            ctx->skipped++;
            continue;
        }
        ctx->line[ctx->line_len++] = (char)c;
    }
    return 0;
}

static void b64_output(void* user, const uint8_t* data, size_t len) {
    ingest_ctx* ctx = (ingest_ctx*)user;
    if (!ctx->failed) {
        ingest_bytes(ctx, data, len);
    }
}

static int dispatch(ingest_ctx* ctx, const uint8_t* data, size_t len) {
    if (ctx->mode == 1) {
        if (subs_b64_feed(&ctx->b64, data, len, b64_output, ctx) != 0) {
            return ingest_fail(ctx, "invalid base64 subscription");
        }
        return ctx->failed ? -1 : 0;
    }
    return ingest_bytes(ctx, data, len);
}

static int ingest_feed(ingest_ctx* ctx, const uint8_t* data, size_t len) {
    if (ctx->failed) {
        return -1;
    }
    if (ctx->mode >= 0) {
        return dispatch(ctx, data, len);
    }
    size_t copy = len;
    if (ctx->sniff_len + copy > sizeof(ctx->sniff)) {
        copy = sizeof(ctx->sniff) - ctx->sniff_len;
    }
    memcpy(ctx->sniff + ctx->sniff_len, data, copy);
    ctx->sniff_len += copy;
    if (ctx->sniff_len < sizeof(ctx->sniff)) {
        return 0;
    }
    ctx->mode = detect_mode(ctx->sniff, ctx->sniff_len);
    if (ctx->mode == 2) {
        return ingest_fail(ctx, "unsupported JSON subscription format");
    }
    if (ctx->mode < 0) {
        return ingest_fail(ctx, "empty subscription response");
    }
    if (ctx->mode == 1) {
        subs_b64_init(&ctx->b64);
    }
    if (dispatch(ctx, ctx->sniff, ctx->sniff_len) != 0) {
        return -1;
    }
    ctx->sniff_len = 0;
    if (copy < len) {
        return dispatch(ctx, data + copy, len - copy);
    }
    return 0;
}

static int ingest_finish(ingest_ctx* ctx) {
    if (ctx->failed) {
        return -1;
    }
    if (ctx->mode < 0) {
        ctx->mode = detect_mode(ctx->sniff, ctx->sniff_len);
        if (ctx->mode == 2) {
            return ingest_fail(ctx, "unsupported JSON subscription format");
        }
        if (ctx->mode < 0) {
            return ingest_fail(ctx, "empty subscription response");
        }
        if (ctx->mode == 1) {
            subs_b64_init(&ctx->b64);
        }
        if (dispatch(ctx, ctx->sniff, ctx->sniff_len) != 0) {
            return -1;
        }
        ctx->sniff_len = 0;
    }
    if (ctx->mode == 1) {
        if (subs_b64_finish(&ctx->b64, b64_output, ctx) != 0) {
            return ingest_fail(ctx, "invalid base64 subscription");
        }
    }
    if (ctx->discard_line) {
        ctx->line_len = 0;
        ctx->discard_line = 0;
    }
    if (ctx->line_len > 0) {
        ctx->line[ctx->line_len] = '\0';
        if (process_line(ctx, ctx->line) != 0) {
            return -1;
        }
        ctx->line_len = 0;
    }
    return ctx->failed ? -1 : 0;
}

static void apply_http_metadata(ingest_ctx* ctx, const mewnet_http_result* http) {
    if (ctx->name[0] == '\0' && http->profile_title[0] != '\0') {
        const char* title = http->profile_title;
        if (strncmp(title, "base64:", 7) == 0) {
            if (subs_b64_decode_string(title + 7, ctx->name, sizeof(ctx->name)) != 0) {
                ctx->name[0] = '\0';
            }
        } else {
            snprintf(ctx->name, sizeof(ctx->name), "%s", title);
        }
    }
    if (http->profile_update_interval[0] != '\0') {
        int hours = atoi(http->profile_update_interval);
        if (hours > 0) {
            int seconds = hours * 3600;
            if (seconds < SUBS_MIN_FETCH_INTERVAL) seconds = SUBS_MIN_FETCH_INTERVAL;
            if (seconds > SUBS_MAX_FETCH_INTERVAL) seconds = SUBS_MAX_FETCH_INTERVAL;
            ctx->fetch_interval_sec = seconds;
        }
    }
    if (ctx->userinfo[0] == '\0' && http->subscription_userinfo[0] != '\0') {
        snprintf(ctx->userinfo, sizeof(ctx->userinfo), "%s", http->subscription_userinfo);
    }
}

static int http_write_cb(const void* data, size_t len, void* user) {
    ingest_ctx* ctx = (ingest_ctx*)user;
    return ingest_feed(ctx, (const uint8_t*)data, len) == 0 ? 0 : 1;
}

static void ingest_free(ingest_ctx* ctx) {
    if (ctx->insert_proxy != NULL) sqlite3_finalize(ctx->insert_proxy);
    if (ctx->select_proxy != NULL) sqlite3_finalize(ctx->select_proxy);
    if (ctx->update_proxy != NULL) sqlite3_finalize(ctx->update_proxy);
    if (ctx->upsert_source != NULL) sqlite3_finalize(ctx->upsert_source);
    ctx->insert_proxy = NULL;
    ctx->select_proxy = NULL;
    ctx->update_proxy = NULL;
    ctx->upsert_source = NULL;
}

static int load_sub_row(mewnet_subs* db, int64_t id, sub_row* row, mewnet_error* err) {
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT id,group_id,url,etag,last_modified,generation,failure_count,fetch_interval_sec"
                        " FROM subscriptions WHERE id=?",
                        err) != 0) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            *err = subs_make_error(MEWNET_ERROR_NOT_FOUND, "subscription %lld not found",
                                   (long long)id);
        } else {
            *err = subs_db_error(db->db, "load subscription");
        }
        return -1;
    }
    memset(row, 0, sizeof(*row));
    row->id = sqlite3_column_int64(stmt, 0);
    row->group_id = sqlite3_column_int64(stmt, 1);
    subs_db_copy_text(stmt, 2, row->url, sizeof(row->url));
    subs_db_copy_text(stmt, 3, row->etag, sizeof(row->etag));
    subs_db_copy_text(stmt, 4, row->last_modified, sizeof(row->last_modified));
    row->generation = sqlite3_column_int64(stmt, 5);
    row->failure_count = sqlite3_column_int(stmt, 6);
    row->fetch_interval_sec = sqlite3_column_int(stmt, 7);
    sqlite3_finalize(stmt);
    return 0;
}

static int update_subscription_success(mewnet_subs* db, const ingest_ctx* ctx,
                                       const mewnet_http_result* http, int64_t now,
                                       mewnet_error* err) {
    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        "UPDATE subscriptions SET"
                        " last_fetch_at=?, next_fetch_at=?, last_status=?, failure_count=0,"
                        " etag=?, last_modified=?, userinfo=CASE WHEN ?<>'' THEN ? ELSE userinfo END,"
                        " name=CASE WHEN ?<>'' THEN ? ELSE name END,"
                        " fetch_interval_sec=?, last_error=''"
                        " WHERE id=?",
                        err) != 0) {
        return -1;
    }
    int64_t next = now + (int64_t)ctx->fetch_interval_sec * 1000;
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_int64(stmt, 2, next);
    sqlite3_bind_int(stmt, 3, http->status);
    subs_db_bind_text(stmt, 4, http->etag);
    subs_db_bind_text(stmt, 5, http->last_modified);
    subs_db_bind_text(stmt, 6, ctx->userinfo);
    subs_db_bind_text(stmt, 7, ctx->userinfo);
    subs_db_bind_text(stmt, 8, ctx->name);
    subs_db_bind_text(stmt, 9, ctx->name);
    sqlite3_bind_int(stmt, 10, ctx->fetch_interval_sec);
    sqlite3_bind_int64(stmt, 11, ctx->subscription_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        *err = subs_db_error(db->db, "update subscription");
        return -1;
    }
    return 0;
}

static int update_subscription_failure(mewnet_subs* db, int64_t id, int failure_count,
                                       int64_t now, int status, const char* message,
                                       mewnet_error* err) {
    int64_t backoff = 300;
    int shift = failure_count > 8 ? 8 : failure_count;
    if (shift > 0) {
        backoff <<= shift;
    }
    if (backoff > 86400) {
        backoff = 86400;
    }
    int64_t next = now + backoff * 1000;

    sqlite3_stmt* stmt = NULL;
    if (subs_db_prepare(db->db, &stmt,
                        "UPDATE subscriptions SET"
                        " last_fetch_at=?, next_fetch_at=?, last_status=?,"
                        " failure_count=failure_count+1, last_error=?"
                        " WHERE id=?",
                        err) != 0) {
        return -1;
    }
    sqlite3_bind_int64(stmt, 1, now);
    sqlite3_bind_int64(stmt, 2, next);
    sqlite3_bind_int(stmt, 3, status);
    subs_db_bind_text(stmt, 4, message);
    sqlite3_bind_int64(stmt, 5, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        *err = subs_db_error(db->db, "update subscription failure");
        return -1;
    }
    return 0;
}

static mewnet_subs_fetch_result fetch_impl(mewnet_subs* db, int64_t subscription_id, bool wait) {
    mewnet_subs_fetch_result result;
    memset(&result, 0, sizeof(result));
    result.subscription_id = subscription_id;
    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }

    sub_row row;
    if (load_sub_row(db, subscription_id, &row, &result.error) != 0) {
        return result;
    }

    if (wait) {
        result.error = mewnet_subs_rate_limit_wait(db);
        if (result.error.code != MEWNET_ERROR_NONE) {
            return result;
        }
    }

    mewnet_http_options http_options;
    memset(&http_options, 0, sizeof(http_options));
    http_options.timeout_ms = 60000;
    http_options.user_agent = db->user_agent;
    http_options.etag = row.etag[0] != '\0' ? row.etag : NULL;
    http_options.last_modified = row.last_modified[0] != '\0' ? row.last_modified : NULL;
    http_options.max_redirects = 5;

    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_exec(db->db, "BEGIN IMMEDIATE;", &err) != 0) {
        result.error = err;
        return result;
    }

    sqlite3_stmt* generation_stmt = NULL;
    int64_t generation = row.generation + 1;
    if (subs_db_prepare(db->db, &generation_stmt,
                        "UPDATE subscriptions SET generation=? WHERE id=?", &err) != 0) {
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }
    sqlite3_bind_int64(generation_stmt, 1, generation);
    sqlite3_bind_int64(generation_stmt, 2, subscription_id);
    if (sqlite3_step(generation_stmt) != SQLITE_DONE) {
        sqlite3_finalize(generation_stmt);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = subs_db_error(db->db, "update generation");
        return result;
    }
    sqlite3_finalize(generation_stmt);

    ingest_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.subs = db;
    ctx.subscription_id = subscription_id;
    ctx.group_id = row.group_id;
    ctx.generation = generation;
    ctx.now = subs_now_ms();
    ctx.fetch_interval_sec = row.fetch_interval_sec > 0 ? row.fetch_interval_sec
                                                        : db->default_fetch_interval_sec;
    ctx.mode = -1;
    subs_clear_error(&ctx.error);

    if (subs_db_prepare(db->db, &ctx.insert_proxy,
                        "INSERT OR IGNORE INTO proxies"
                        "(group_id,link,link_hash,name,protocol,server,port)"
                        " VALUES(?,?,?,?,?,?,?)",
                        &err) != 0 ||
        subs_db_prepare(db->db, &ctx.select_proxy,
                        "SELECT id FROM proxies WHERE group_id=? AND link_hash=?",
                        &err) != 0 ||
        subs_db_prepare(db->db, &ctx.update_proxy,
                        "UPDATE proxies SET"
                        " name=CASE WHEN name='' THEN ? ELSE name END,"
                        " protocol=CASE WHEN protocol='' THEN ? ELSE protocol END,"
                        " server=CASE WHEN server='' THEN ? ELSE server END,"
                        " port=CASE WHEN port=0 THEN ? ELSE port END,"
                        " manual=0 WHERE id=?",
                        &err) != 0 ||
        subs_db_prepare(db->db, &ctx.upsert_source,
                        "INSERT INTO proxy_sources(proxy_id,subscription_id,last_seen_generation)"
                        " VALUES(?,?,?)"
                        " ON CONFLICT(proxy_id,subscription_id) DO UPDATE SET"
                        " last_seen_generation=excluded.last_seen_generation",
                        &err) != 0) {
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }

    mewnet_http_result http = mewnet_http_stream(row.url, &http_options, http_write_cb, &ctx);
    result.http_status = http.status;
    if (!http.ok) {
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        if (ctx.failed && ctx.error.code != MEWNET_ERROR_NONE) {
            result.error = ctx.error;
        } else {
            result.error = http.error;
        }
        mewnet_error update_err;
        subs_clear_error(&update_err);
        int64_t now = subs_now_ms();
        update_subscription_failure(db, subscription_id, row.failure_count, now,
                                    http.status, result.error.message, &update_err);
        return result;
    }

    if (http.status == 304) {
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        int64_t now = subs_now_ms();
        int64_t next = now + (int64_t)ctx.fetch_interval_sec * 1000;
        sqlite3_stmt* stmt = NULL;
        if (subs_db_prepare(db->db, &stmt,
                            "UPDATE subscriptions SET last_fetch_at=?, next_fetch_at=?,"
                            " last_status=304, failure_count=0, last_error='' WHERE id=?",
                            &err) == 0) {
            sqlite3_bind_int64(stmt, 1, now);
            sqlite3_bind_int64(stmt, 2, next);
            sqlite3_bind_int64(stmt, 3, subscription_id);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                result.error = subs_db_error(db->db, "update subscription 304");
            }
            sqlite3_finalize(stmt);
        } else {
            result.error = err;
        }
        return result;
    }

    if (ingest_finish(&ctx) != 0) {
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = ctx.error;
        int64_t now = subs_now_ms();
        mewnet_error update_err;
        subs_clear_error(&update_err);
        update_subscription_failure(db, subscription_id, row.failure_count, now,
                                    http.status, result.error.message, &update_err);
        return result;
    }

    apply_http_metadata(&ctx, &http);

    if (ctx.added + ctx.updated == 0) {
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = subs_make_error(MEWNET_ERROR_FAILED,
                                       "subscription contains no configs");
        int64_t now = subs_now_ms();
        mewnet_error update_err;
        subs_clear_error(&update_err);
        update_subscription_failure(db, subscription_id, row.failure_count, now,
                                    http.status, result.error.message, &update_err);
        return result;
    }

    sqlite3_stmt* delete_stmt = NULL;
    if (subs_db_prepare(db->db, &delete_stmt,
                        "DELETE FROM proxy_sources WHERE subscription_id=?"
                        " AND last_seen_generation<>?",
                        &err) != 0) {
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }
    sqlite3_bind_int64(delete_stmt, 1, subscription_id);
    sqlite3_bind_int64(delete_stmt, 2, generation);
    if (sqlite3_step(delete_stmt) != SQLITE_DONE) {
        sqlite3_finalize(delete_stmt);
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = subs_db_error(db->db, "delete stale proxy sources");
        return result;
    }
    sqlite3_finalize(delete_stmt);

    if (subs_db_prepare(db->db, &delete_stmt,
                        "DELETE FROM proxies WHERE group_id=? AND manual=0"
                        " AND NOT EXISTS (SELECT 1 FROM proxy_sources ps WHERE ps.proxy_id=proxies.id)",
                        &err) != 0) {
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }
    sqlite3_bind_int64(delete_stmt, 1, row.group_id);
    if (sqlite3_step(delete_stmt) != SQLITE_DONE) {
        sqlite3_finalize(delete_stmt);
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = subs_db_error(db->db, "delete orphan proxies");
        return result;
    }
    result.removed = (size_t)sqlite3_changes(db->db);
    sqlite3_finalize(delete_stmt);

    ctx.now = subs_now_ms();
    if (update_subscription_success(db, &ctx, &http, ctx.now, &err) != 0) {
        ingest_free(&ctx);
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }

    ingest_free(&ctx);
    if (subs_db_exec(db->db, "COMMIT;", &err) != 0) {
        sqlite3_exec(db->db, "ROLLBACK;", NULL, NULL, NULL);
        result.error = err;
        return result;
    }

    result.added = ctx.added;
    result.updated = ctx.updated;
    result.skipped = ctx.skipped;
    result.error = subs_make_error(MEWNET_ERROR_NONE, "");
    return result;
}

mewnet_subs_fetch_result mewnet_subs_fetch(mewnet_subs* db, int64_t subscription_id) {
    return fetch_impl(db, subscription_id, true);
}

mewnet_subs_fetch_result mewnet_subs_fetch_now(mewnet_subs* db, int64_t subscription_id) {
    return fetch_impl(db, subscription_id, false);
}

mewnet_subs_fetch_result mewnet_subs_fetch_due(mewnet_subs* db) {
    mewnet_subs_fetch_result result;
    memset(&result, 0, sizeof(result));
    if (db == NULL) {
        result.error = subs_make_error(MEWNET_ERROR_INVALID_ARGUMENT, "db is required");
        return result;
    }
    sqlite3_stmt* stmt = NULL;
    mewnet_error err;
    subs_clear_error(&err);
    if (subs_db_prepare(db->db, &stmt,
                        "SELECT s.id FROM subscriptions s"
                        " JOIN subscription_groups g ON g.id=s.group_id"
                        " WHERE s.enabled=1 AND g.enabled=1 AND s.next_fetch_at<=?"
                        " ORDER BY g.priority DESC, s.priority DESC,"
                        " s.next_fetch_at ASC, s.id ASC LIMIT 1",
                        &err) != 0) {
        result.error = err;
        return result;
    }
    sqlite3_bind_int64(stmt, 1, subs_now_ms());
    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        if (rc == SQLITE_DONE) {
            result.error = subs_make_error(MEWNET_ERROR_NONE, "");
            return result;
        }
        result.error = subs_db_error(db->db, "select due subscription");
        return result;
    }
    int64_t id = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return mewnet_subs_fetch(db, id);
}
