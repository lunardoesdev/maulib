#include "milib_subs_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

milib_error subs_make_error(milib_error_code code, const char* format, ...) {
    milib_error err;
    memset(&err, 0, sizeof(err));
    err.code = code;
    va_list args;
    va_start(args, format);
    vsnprintf(err.message, sizeof(err.message), format, args);
    va_end(args);
    return err;
}

milib_error subs_db_error(sqlite3* db, const char* context) {
    return subs_make_error(MILIB_ERROR_FAILED, "%s: %s", context,
                           db != NULL ? sqlite3_errmsg(db) : "sqlite error");
}

void subs_clear_error(milib_error* err) {
    if (err != NULL) {
        memset(err, 0, sizeof(*err));
        err->code = MILIB_ERROR_NONE;
    }
}

bool subs_error_ok(const milib_error* err) {
    return err == NULL || err->code == MILIB_ERROR_NONE;
}

int subs_db_exec(sqlite3* db, const char* sql, milib_error* err) {
    char* message = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &message);
    if (rc != SQLITE_OK) {
        if (err != NULL) {
            *err = subs_make_error(MILIB_ERROR_FAILED, "sqlite: %s",
                                   message != NULL ? message : sqlite3_errmsg(db));
        }
        sqlite3_free(message);
        return -1;
    }
    sqlite3_free(message);
    return 0;
}

int subs_db_prepare(sqlite3* db, sqlite3_stmt** stmt, const char* sql, milib_error* err) {
    int rc = sqlite3_prepare_v2(db, sql, -1, stmt, NULL);
    if (rc != SQLITE_OK) {
        if (err != NULL) {
            *err = subs_make_error(MILIB_ERROR_FAILED, "sqlite prepare: %s",
                                   sqlite3_errmsg(db));
        }
        return -1;
    }
    return 0;
}

void subs_db_bind_text(sqlite3_stmt* stmt, int index, const char* value) {
    sqlite3_bind_text(stmt, index, value != NULL ? value : "", -1, SQLITE_TRANSIENT);
}

void subs_db_bind_blob(sqlite3_stmt* stmt, int index, const void* data, int size) {
    sqlite3_bind_blob(stmt, index, data, size, SQLITE_TRANSIENT);
}

const char* subs_db_column_text(sqlite3_stmt* stmt, int column) {
    const unsigned char* text = sqlite3_column_text(stmt, column);
    return text != NULL ? (const char*)text : "";
}

void subs_db_copy_text(sqlite3_stmt* stmt, int column, char* dst, size_t dst_size) {
    if (dst_size == 0) {
        return;
    }
    const char* text = subs_db_column_text(stmt, column);
    size_t len = strlen(text);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, text, len);
    dst[len] = '\0';
}

int64_t subs_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int64_t subs_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int subs_today_utc(void) {
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    return (tm.tm_year + 1900) * 10000 + (tm.tm_mon + 1) * 100 + tm.tm_mday;
}

void subs_sleep_ms(int64_t ms) {
    if (ms <= 0) {
        return;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(ms / 1000);
    ts.tv_nsec = (long)((ms % 1000) * 1000000);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

char* subs_trim(char* value) {
    while (*value != '\0' && isspace((unsigned char)*value)) {
        value++;
    }
    size_t len = strlen(value);
    while (len > 0 && isspace((unsigned char)value[len - 1])) {
        value[--len] = '\0';
    }
    return value;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

size_t subs_percent_decode(const char* src, char* dst, size_t dst_size) {
    size_t out = 0;
    while (src != NULL && *src != '\0' && out + 1 < dst_size) {
        if (src[0] == '%' && src[1] != '\0' && src[2] != '\0') {
            int hi = hex_value(src[1]);
            int lo = hex_value(src[2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                src += 3;
                continue;
            }
        }
        dst[out++] = *src++;
    }
    if (dst_size > 0) {
        dst[out] = '\0';
    }
    return out;
}

void subs_link_hint(const char* link,
                    char* name, size_t name_size,
                    char* protocol, size_t protocol_size,
                    char* server, size_t server_size,
                    int* port) {
    if (name_size > 0) name[0] = '\0';
    if (protocol_size > 0) protocol[0] = '\0';
    if (server_size > 0) server[0] = '\0';
    if (port != NULL) *port = 0;
    if (link == NULL) {
        return;
    }

    const char* scheme_end = strstr(link, "://");
    if (scheme_end == NULL) {
        return;
    }
    size_t scheme_len = (size_t)(scheme_end - link);
    if (scheme_len + 1 > protocol_size) {
        scheme_len = protocol_size > 0 ? protocol_size - 1 : 0;
    }
    if (protocol_size > 0) {
        memcpy(protocol, link, scheme_len);
        protocol[scheme_len] = '\0';
    }

    const char* fragment = strchr(link, '#');
    if (fragment != NULL && fragment[1] != '\0' && name_size > 0) {
        subs_percent_decode(fragment + 1, name, name_size);
    }

    if (strcmp(protocol, "vmess") == 0 || strcmp(protocol, "vmess1") == 0) {
        return;
    }

    const char* authority = scheme_end + 3;
    const char* authority_end = authority + strcspn(authority, "/?#");
    const char* at = NULL;
    for (const char* p = authority; p < authority_end; p++) {
        if (*p == '@') {
            at = p;
        }
    }
    if (at != NULL) {
        authority = at + 1;
    }
    if (authority >= authority_end) {
        return;
    }

    const char* host_start = authority;
    const char* host_end = authority_end;
    const char* port_start = NULL;
    if (*host_start == '[') {
        const char* close = memchr(host_start, ']', (size_t)(authority_end - host_start));
        if (close != NULL) {
            host_end = close + 1;
            if (host_end < authority_end && *host_end == ':') {
                port_start = host_end + 1;
            }
        }
    } else {
        const char* colon = NULL;
        for (const char* p = authority; p < authority_end; p++) {
            if (*p == ':') {
                colon = p;
            }
        }
        if (colon != NULL) {
            host_end = colon;
            port_start = colon + 1;
        }
    }

    size_t host_len = (size_t)(host_end - host_start);
    if (host_len + 1 > server_size) {
        host_len = server_size > 0 ? server_size - 1 : 0;
    }
    if (server_size > 0) {
        memcpy(server, host_start, host_len);
        server[host_len] = '\0';
    }
    if (port_start != NULL && port_start < authority_end && port != NULL) {
        char tmp[16];
        size_t n = (size_t)(authority_end - port_start);
        if (n >= sizeof(tmp)) n = sizeof(tmp) - 1;
        memcpy(tmp, port_start, n);
        tmp[n] = '\0';
        *port = atoi(tmp);
    }

    if (name[0] == '\0' && name_size > 0) {
        if (server[0] != '\0' && *port > 0) {
            snprintf(name, name_size, "%s://%s:%d", protocol, server, *port);
        } else if (server[0] != '\0') {
            snprintf(name, name_size, "%s://%s", protocol, server);
        } else {
            snprintf(name, name_size, "%s", protocol);
        }
    }
}

milib_error milib_subs_rate_limit_wait(milib_subs* db) {
    if (db == NULL) {
        return subs_make_error(MILIB_ERROR_INVALID_ARGUMENT, "database is required");
    }
    if (db->request_limit <= 0 || db->request_window_seconds <= 0) {
        return subs_make_error(MILIB_ERROR_NONE, "");
    }

    int64_t interval_ms = ((int64_t)db->request_window_seconds * 1000) / db->request_limit;
    if (interval_ms < 100) {
        interval_ms = 100;
    }

    pthread_mutex_lock(&db->rate_lock);
    int64_t now = subs_monotonic_ms();
    if (db->next_request_ms > now) {
        subs_sleep_ms(db->next_request_ms - now);
        now = subs_monotonic_ms();
    }
    int64_t jitter = interval_ms / 5;
    int64_t offset = 0;
    if (jitter > 0) {
        offset = (int64_t)(rand_r(&db->rng) % (unsigned int)(jitter * 2 + 1)) - jitter;
    }
    db->next_request_ms = now + interval_ms + offset;
    pthread_mutex_unlock(&db->rate_lock);

    return subs_make_error(MILIB_ERROR_NONE, "");
}
