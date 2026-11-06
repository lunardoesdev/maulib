#ifndef MEWNET_SUBS_INTERNAL_H
#define MEWNET_SUBS_INTERNAL_H

#ifdef _WIN32
#include <windows.h>
typedef CRITICAL_SECTION mewnet_mutex;
#else
#include <pthread.h>
typedef pthread_mutex_t mewnet_mutex;
#endif

#include <sqlite3.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mewnet_subs.h"

#define SUBS_LINE_MAX 16384
#define SUBS_SNIFF_MAX 512
#define SUBS_ERROR_MAX MEWNET_ERROR_MAX
#define SUBS_DEFAULT_FETCH_INTERVAL 21600
#define SUBS_MIN_FETCH_INTERVAL 300
#define SUBS_MAX_FETCH_INTERVAL 604800
#define SUBS_DEFAULT_TEST_URL "https://www.gstatic.com/generate_204"
#define SUBS_DEFAULT_TEST_EXPECTED "204"
#define SUBS_DEFAULT_TEST_TIMEOUT_MS 5000

struct mewnet_subs {
    sqlite3* db;
    char db_path[4096];
    char user_agent[256];
    char test_url[MEWNET_URL_MAX];
    char test_expected[64];
    int test_timeout_ms;
    int request_limit;
    int request_window_seconds;
    int default_fetch_interval_sec;

    mewnet_mutex rate_lock;
    int64_t next_request_ms;
    unsigned int rng;
    volatile int cancel_fetch;
};

/* Error helpers. */
mewnet_error subs_make_error(mewnet_error_code code, const char* format, ...);
mewnet_error subs_db_error(sqlite3* db, const char* context);
void subs_clear_error(mewnet_error* err);
bool subs_error_ok(const mewnet_error* err);

/* SQLite helpers. */
int subs_db_exec(sqlite3* db, const char* sql, mewnet_error* err);
int subs_db_prepare(sqlite3* db, sqlite3_stmt** stmt, const char* sql, mewnet_error* err);
void subs_db_bind_text(sqlite3_stmt* stmt, int index, const char* value);
void subs_db_bind_blob(sqlite3_stmt* stmt, int index, const void* data, int size);
const char* subs_db_column_text(sqlite3_stmt* stmt, int column);
void subs_db_copy_text(sqlite3_stmt* stmt, int column, char* dst, size_t dst_size);

/* Portability helpers. */
void subs_mutex_init(mewnet_mutex* mutex);
void subs_mutex_destroy(mewnet_mutex* mutex);
void subs_mutex_lock(mewnet_mutex* mutex);
void subs_mutex_unlock(mewnet_mutex* mutex);

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

/* Config identity: full link without the display-name fragment. */
void subs_link_hash(const char* link, uint8_t out[32]);
/* Protocol+host+port+credentials identity (SHA-256). */
void subs_link_identity(const char* link, uint8_t out[32]);

/* Rate limiting. */
int64_t subs_rate_limit_wait(mewnet_subs* db);

#endif /* MEWNET_SUBS_INTERNAL_H */
