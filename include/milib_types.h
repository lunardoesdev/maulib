#ifndef MILIB_TYPES_H
#define MILIB_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MILIB_ABI_VERSION 2

#define MILIB_NAME_MAX 512
#define MILIB_TYPE_MAX 32
#define MILIB_HOST_MAX 256
#define MILIB_ADDR_MAX 64
#define MILIB_AUTH_MAX 512
#define MILIB_URL_MAX 2048
#define MILIB_LINK_MAX 8192
#define MILIB_IP_MAX 64
#define MILIB_ERROR_MAX 512

typedef enum milib_error_code {
    MILIB_ERROR_NONE = 0,
    MILIB_ERROR_FAILED,
    MILIB_ERROR_INVALID_ARGUMENT,
    MILIB_ERROR_NOT_FOUND,
    MILIB_ERROR_TIMEOUT,
    MILIB_ERROR_CANCELLED,
    MILIB_ERROR_CLOSED
} milib_error_code;

typedef struct milib_error {
    milib_error_code code;
    char message[MILIB_ERROR_MAX];
} milib_error;

typedef enum milib_log_level {
    MILIB_LOG_DEBUG = 0,
    MILIB_LOG_INFO,
    MILIB_LOG_WARNING,
    MILIB_LOG_ERROR,
    MILIB_LOG_SILENT
} milib_log_level;

typedef void (*milib_log_cb)(milib_log_level level, const char* message, void* user);

/* Opaque objects. */
typedef struct milib_instance milib_instance;
typedef struct milib_link milib_link;
typedef struct milib_task milib_task;

/* Lifecycle. */
typedef struct milib_config {
    int mixed_port;          /* 0: YAML value or free port, -1: disabled */
    const char* home_dir;    /* NULL: temporary directory */
    const char* config_yaml; /* NULL: built-in defaults */
    const char* log_level;   /* NULL: library default */
} milib_config;

typedef struct milib_init_result {
    milib_instance* instance;
    milib_error error;
} milib_init_result;

typedef struct milib_status_info {
    bool ok;
    milib_error_code error;
    char version[MILIB_TYPE_MAX];
    char mihomo_version[MILIB_TYPE_MAX];
    char mode[MILIB_TYPE_MAX];
    char selected[MILIB_NAME_MAX];
    bool running;
    int mixed_port;
    int socks_port;
    int http_port;
    int proxies;
    unsigned goroutines;
    unsigned long long rss_bytes;
    unsigned long long uptime_ms;
} milib_status_info;

/* Proxy management. */
typedef struct milib_proxy_info {
    char name[MILIB_NAME_MAX];
    char proxy_type[MILIB_TYPE_MAX];
    char server[MILIB_HOST_MAX];
    int port;
    char source[MILIB_TYPE_MAX];
    bool alive;
    unsigned delay_ms;
} milib_proxy_info;

typedef struct milib_proxy_list {
    bool ok;
    milib_error_code error;
    milib_proxy_info* items;
    size_t count;
    char selected[MILIB_NAME_MAX];
} milib_proxy_list;

typedef struct milib_add_options {
    const char* name;
    bool select_proxy;
    bool skip_cert_verify;
} milib_add_options;

/* SharedVPNLink. */
typedef struct milib_link_options {
    const char* name;
    bool select_proxy;
    bool local_proxy;
    const char* username;
    const char* password;
    const char* proxy_password;
} milib_link_options;

typedef struct milib_link_result {
    milib_link* link;
    milib_error error;
} milib_link_result;

typedef struct milib_link_info {
    bool ok;
    milib_error_code error;
    uint64_t id;
    char name[MILIB_NAME_MAX];
    char proxy_type[MILIB_TYPE_MAX];
    char server[MILIB_HOST_MAX];
    int port;
    char uri[MILIB_LINK_MAX];
    char socks_addr[MILIB_ADDR_MAX];
    char socks_username[MILIB_AUTH_MAX];
    char socks_password[MILIB_AUTH_MAX];
    bool selected;
} milib_link_info;

typedef struct milib_link_list {
    bool ok;
    milib_error_code error;
    milib_link_info* items;
    size_t count;
} milib_link_list;

/* Latency, throughput and exit IP. */
typedef struct milib_delay_options {
    const char* url;
    int timeout_ms;
    const char* expected;
} milib_delay_options;

typedef struct milib_delay_result {
    bool ok;
    milib_error_code error;
    char name[MILIB_NAME_MAX];
    char proxy_type[MILIB_TYPE_MAX];
    char server[MILIB_HOST_MAX];
    int port;
    char url[MILIB_URL_MAX];
    unsigned delay_ms;
} milib_delay_result;

typedef struct milib_ping_result {
    bool ok;
    milib_error_code error;
    unsigned delay_ms;
} milib_ping_result;

typedef struct milib_speed_options {
    const char* url;
    long long bytes;
    int timeout_ms;
} milib_speed_options;

typedef struct milib_speed_result {
    bool ok;
    milib_error_code error;
    int status;
    long long bytes;
    double seconds;
    double bytes_per_second;
    double mbps;
} milib_speed_result;

typedef enum milib_exit_ip_format {
    MILIB_EXIT_IP_TEXT = 0,
    MILIB_EXIT_IP_JSON = 1
} milib_exit_ip_format;

typedef struct milib_exit_ip_options {
    const char* url;
    milib_exit_ip_format format;
    int timeout_ms;
} milib_exit_ip_options;

typedef struct milib_exit_ip_result {
    bool ok;
    milib_error_code error;
    int status;
    bool valid;
    char ip[MILIB_IP_MAX];
} milib_exit_ip_result;

typedef struct milib_ping_many_result {
    bool ok;
    milib_error_code error;
    milib_ping_result* items;
    size_t count;
} milib_ping_many_result;

/* Detailed result of parsing and testing a raw share link. */
typedef struct milib_link_test_result {
    bool ok;
    milib_error error;
    char name[MILIB_NAME_MAX];
    char proxy_type[MILIB_TYPE_MAX];
    char server[MILIB_HOST_MAX];
    int port;
    unsigned delay_ms;
} milib_link_test_result;

/* Streaming HTTP. */
typedef int (*milib_http_cb)(const void* data, size_t len, void* user);

typedef struct milib_http_options {
    int timeout_ms;
    const char* user_agent;
    const char* etag;
    const char* last_modified;
    int max_redirects;
} milib_http_options;

typedef struct milib_http_result {
    bool ok;
    milib_error error;
    int status;
    char etag[256];
    char last_modified[256];
    char subscription_userinfo[512];
    char profile_title[256];
    char profile_update_interval[32];
    unsigned long long bytes;
} milib_http_result;

/* Async tasks. */
typedef enum milib_task_kind {
    MILIB_TASK_NONE = 0,
    MILIB_TASK_DELAY,
    MILIB_TASK_PING,
    MILIB_TASK_SPEED,
    MILIB_TASK_EXIT_IP,
    MILIB_TASK_PING_MANY
} milib_task_kind;

typedef enum milib_task_state {
    MILIB_TASK_RUNNING = 0,
    MILIB_TASK_DONE,
    MILIB_TASK_CANCELLED
} milib_task_state;

typedef struct milib_task_result {
    milib_task_state state;
    milib_task_kind kind;
    milib_error_code error;
    milib_delay_result delay;
    milib_ping_result ping;
    milib_speed_result speed;
    milib_exit_ip_result exit_ip;
    const milib_ping_result* pings;
    size_t ping_count;
} milib_task_result;

typedef void (*milib_task_cb)(const milib_task_result* result, void* user);

#ifdef __cplusplus
}
#endif

#endif /* MILIB_TYPES_H */
