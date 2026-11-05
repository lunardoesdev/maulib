#ifndef MILIB_SUBS_INTERNAL_H
#define MILIB_SUBS_INTERNAL_H

#include <pthread.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "milib_subs.h"

#define SUBS_LINE_MAX 16384
#define SUBS_SNIFF_MAX 512
#define SUBS_ERROR_MAX MILIB_ERROR_MAX
#define SUBS_DEFAULT_FETCH_INTERVAL 21600
#define SUBS_MIN_FETCH_INTERVAL 300
#define SUBS_MAX_FETCH_INTERVAL 604800
#define SUBS_DEFAULT_TEST_URL "https://www.gstatic.com/generate_204"
#define SUBS_DEFAULT_TEST_EXPECTED "204"
#define SUBS_DEFAULT_TEST_TIMEOUT_MS 5000

struct milib_subs {
    sqlite3* db;
    char user_agent[256];
    char test_url[MILIB_URL_MAX];
    char test_expected[64];
    int test_timeout_ms;
    int request_limit;
    int request_window_seconds;
    int default_fetch_interval_sec;

    pthread_mutex_t rate_lock;
    int64_t next_request_ms;
    unsigned int rng;
};

/* Error helpers. */
milib_error subs_make_error(milib_error_code code, const char* format, ...);
milib_error subs_db_error(sqlite3* db, const char* context);
void subs_clear_error(milib_error* err);
bool subs_error_ok(const milib_error* err);

/* SQLite helpers. */
int subs_db_exec(sqlite3* db, const char* sql, milib_error* err);
int subs_db_prepare(sqlite3* db, sqlite3_stmt** stmt, const char* sql, milib_error* err);
void subs_db_bind_text(sqlite3_stmt* stmt, int index, const char* value);
void subs_db_bind_blob(sqlite3_stmt* stmt, int index, const void* data, int size);
const char* subs_db_column_text(sqlite3_stmt* stmt, int column);
void subs_db_copy_text(sqlite3_stmt* stmt, int column, char* dst, size_t dst_size);

/* Time. */
int64_t subs_now_ms(void);
int64_t subs_monotonic_ms(void);
int subs_today_utc(void);
void subs_sleep_ms(int64_t ms);

/* Small string helpers. */
char* subs_trim(char* value);
size_t subs_percent_decode(const char* src, char* dst, size_t dst_size);
void subs_link_hint(const char* link,
                    char* name, size_t name_size,
                    char* protocol, size_t protocol_size,
                    char* server, size_t server_size,
                    int* port);

/* Streaming base64. */
typedef void (*subs_b64_out)(void* user, const uint8_t* data, size_t len);

typedef struct subs_b64 {
    uint8_t q[4];
    int n;
    int pad;
    int finished;
} subs_b64;

void subs_b64_init(subs_b64* state);
int subs_b64_feed(subs_b64* state, const uint8_t* data, size_t len, subs_b64_out out, void* user);
int subs_b64_finish(subs_b64* state, subs_b64_out out, void* user);

int subs_b64_decode_string(const char* input, char* output, size_t output_size);

/* SHA-256. */
void subs_sha256(const void* data, size_t len, uint8_t out[32]);

/* Rate limiting. */
int64_t subs_rate_limit_wait(milib_subs* db);

#endif /* MILIB_SUBS_INTERNAL_H */
