#ifndef MEWNET_TYPES_H
#define MEWNET_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MEWNET_ABI_VERSION 3

#define MEWNET_NAME_MAX 512
#define MEWNET_TYPE_MAX 32
#define MEWNET_HOST_MAX 256
#define MEWNET_ADDR_MAX 64
#define MEWNET_AUTH_MAX 512
#define MEWNET_URL_MAX 2048
#define MEWNET_LINK_MAX 8192
#define MEWNET_IP_MAX 64
#define MEWNET_ERROR_MAX 512

typedef enum mewnet_error_code {
    MEWNET_ERROR_NONE = 0,
    MEWNET_ERROR_FAILED,
    MEWNET_ERROR_INVALID_ARGUMENT,
    MEWNET_ERROR_NOT_FOUND,
    MEWNET_ERROR_TIMEOUT,
    MEWNET_ERROR_CANCELLED,
    MEWNET_ERROR_CLOSED
} mewnet_error_code;

typedef struct mewnet_error {
    mewnet_error_code code;
    char message[MEWNET_ERROR_MAX];
} mewnet_error;

typedef enum mewnet_log_level {
    MEWNET_LOG_DEBUG = 0,
    MEWNET_LOG_INFO,
    MEWNET_LOG_WARNING,
    MEWNET_LOG_ERROR,
    MEWNET_LOG_SILENT
} mewnet_log_level;

typedef void (*mewnet_log_cb)(mewnet_log_level level, const char* message, void* user);

/* Opaque objects. */
typedef struct mewnet_instance mewnet_instance;
typedef struct mewnet_link mewnet_link;
typedef struct mewnet_task mewnet_task;

/* Lifecycle. */
typedef struct mewnet_config {
    int mixed_port;          /* 0: YAML value or free port, -1: disabled */
    const char* home_dir;    /* NULL: temporary directory */
    const char* config_yaml; /* NULL: built-in defaults */
    const char* log_level;   /* NULL: library default */
} mewnet_config;

typedef struct mewnet_init_result {
    mewnet_instance* instance;
    mewnet_error error;
} mewnet_init_result;

typedef struct mewnet_status_info {
    bool ok;
    mewnet_error_code error;
    char version[MEWNET_TYPE_MAX];
    char mihomo_version[MEWNET_TYPE_MAX];
    char mode[MEWNET_TYPE_MAX];
    char selected[MEWNET_NAME_MAX];
    bool running;
    int mixed_port;
    int socks_port;
    int http_port;
    int proxies;
    unsigned goroutines;
    unsigned long long rss_bytes;
    unsigned long long uptime_ms;
} mewnet_status_info;

/* Proxy management. */
typedef struct mewnet_proxy_info {
    char name[MEWNET_NAME_MAX];
    char proxy_type[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port;
    char source[MEWNET_TYPE_MAX];
    bool alive;
    unsigned delay_ms;
} mewnet_proxy_info;

typedef struct mewnet_proxy_list {
    bool ok;
    mewnet_error_code error;
    mewnet_proxy_info* items;
    size_t count;
    char selected[MEWNET_NAME_MAX];
} mewnet_proxy_list;

typedef struct mewnet_add_options {
    const char* name;
    bool select_proxy;
    bool skip_cert_verify;
} mewnet_add_options;

/* SharedVPNLink. */
typedef struct mewnet_link_options {
    const char* name;
    bool select_proxy;
    bool local_proxy;
    const char* username;
    const char* password;
    const char* proxy_password;
} mewnet_link_options;

typedef struct mewnet_link_result {
    mewnet_link* link;
    mewnet_error error;
} mewnet_link_result;

typedef struct mewnet_link_info {
    bool ok;
    mewnet_error_code error;
    uint64_t id;
    char name[MEWNET_NAME_MAX];
    char proxy_type[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port;
    char uri[MEWNET_LINK_MAX];
    char socks_addr[MEWNET_ADDR_MAX];
    char socks_username[MEWNET_AUTH_MAX];
    char socks_password[MEWNET_AUTH_MAX];
    bool selected;
} mewnet_link_info;

typedef struct mewnet_link_list {
    bool ok;
    mewnet_error_code error;
    mewnet_link_info* items;
    size_t count;
} mewnet_link_list;

/* Latency, throughput and exit IP. */
typedef struct mewnet_delay_options {
    const char* url;
    int timeout_ms;
    const char* expected;
} mewnet_delay_options;

typedef struct mewnet_delay_result {
    bool ok;
    mewnet_error_code error;
    char name[MEWNET_NAME_MAX];
    char proxy_type[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port;
    char url[MEWNET_URL_MAX];
    unsigned delay_ms;
    unsigned ttfb_ms;
    unsigned ttlb_ms;
    unsigned long long bytes;
    unsigned long long speed_bps;
} mewnet_delay_result;

typedef struct mewnet_ping_result {
    bool ok;
    mewnet_error_code error;
    unsigned delay_ms;
    unsigned ttfb_ms;
    unsigned ttlb_ms;
    unsigned long long bytes;
    unsigned long long speed_bps;
} mewnet_ping_result;

typedef struct mewnet_speed_options {
    const char* url;
    long long bytes;
    int timeout_ms;
} mewnet_speed_options;

typedef struct mewnet_speed_result {
    bool ok;
    mewnet_error_code error;
    int status;
    long long bytes;
    double seconds;
    double bytes_per_second;
    double mbps;
} mewnet_speed_result;

typedef enum mewnet_exit_ip_format {
    MEWNET_EXIT_IP_TEXT = 0,
    MEWNET_EXIT_IP_JSON = 1
} mewnet_exit_ip_format;

typedef struct mewnet_exit_ip_options {
    const char* url;
    mewnet_exit_ip_format format;
    int timeout_ms;
} mewnet_exit_ip_options;

typedef struct mewnet_exit_ip_result {
    bool ok;
    mewnet_error_code error;
    int status;
    bool valid;
    char ip[MEWNET_IP_MAX];
} mewnet_exit_ip_result;

typedef struct mewnet_ping_many_result {
    bool ok;
    mewnet_error_code error;
    mewnet_ping_result* items;
    size_t count;
} mewnet_ping_many_result;

/* Detailed result of parsing and testing a raw share link. */
typedef struct mewnet_link_test_result {
    bool ok;
    mewnet_error error;
    char name[MEWNET_NAME_MAX];
    char proxy_type[MEWNET_TYPE_MAX];
    char server[MEWNET_HOST_MAX];
    int port;
    unsigned delay_ms;
    unsigned ttfb_ms;
    unsigned ttlb_ms;
    unsigned long long bytes;
    unsigned long long speed_bps;
} mewnet_link_test_result;

typedef struct mewnet_probe_result {
    bool ok;
    mewnet_error error;
    int status;
    unsigned delay_ms;
    unsigned ttfb_ms;
    unsigned ttlb_ms;
    unsigned long long bytes;
    unsigned long long speed_bps;
} mewnet_probe_result;

typedef struct mewnet_fingerprint_result {
    bool ok;
    mewnet_error error;
    uint8_t hash[32];
} mewnet_fingerprint_result;

/* Batch link testing. */
typedef struct mewnet_batch_test_result {
    bool ok;
    mewnet_error error;
    mewnet_link_test_result* items;
    size_t count;
} mewnet_batch_test_result;

/* Streaming HTTP. */
typedef int (*mewnet_http_cb)(const void* data, size_t len, void* user);

typedef struct mewnet_http_options {
    int timeout_ms;
    const char* user_agent;
    const char* etag;
    const char* last_modified;
    int max_redirects;
} mewnet_http_options;

typedef struct mewnet_http_result {
    bool ok;
    mewnet_error error;
    int status;
    char etag[256];
    char last_modified[256];
    char subscription_userinfo[512];
    char profile_title[256];
    char profile_update_interval[32];
    unsigned long long bytes;
} mewnet_http_result;

/* Async tasks. */
typedef enum mewnet_task_kind {
    MEWNET_TASK_NONE = 0,
    MEWNET_TASK_DELAY,
    MEWNET_TASK_PING,
    MEWNET_TASK_SPEED,
    MEWNET_TASK_EXIT_IP,
    MEWNET_TASK_PING_MANY
} mewnet_task_kind;

typedef enum mewnet_task_state {
    MEWNET_TASK_RUNNING = 0,
    MEWNET_TASK_DONE,
    MEWNET_TASK_CANCELLED
} mewnet_task_state;

typedef struct mewnet_task_result {
    mewnet_task_state state;
    mewnet_task_kind kind;
    mewnet_error_code error;
    mewnet_delay_result delay;
    mewnet_ping_result ping;
    mewnet_speed_result speed;
    mewnet_exit_ip_result exit_ip;
    const mewnet_ping_result* pings;
    size_t ping_count;
} mewnet_task_result;

typedef void (*mewnet_task_cb)(const mewnet_task_result* result, void* user);

#ifdef __cplusplus
}
#endif

#endif /* MEWNET_TYPES_H */
