#ifndef MILIB_H
#define MILIB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint64_t milib_handle;

/*
 * Async task callback.
 *
 * Called from a Go goroutine with a JSON payload. result_json is only valid
 * for the duration of the call; copy it if you need to keep it. The callback
 * must not call milib_shutdown().
 */
typedef void (*milib_task_cb)(int64_t task_id, const char* result_json, void* user);

/*
 * Opaque SharedVPNLink handle.
 *
 * Created by milib_link_create() and released by milib_link_destroy(). The
 * handle owns a proxy in the MILIB group and an authenticated local SOCKS5
 * endpoint. Do not use it after destroy.
 */
typedef struct milib_link milib_link;

/* Lifecycle. */

/*
 * Initialize the process-wide mihomo instance.
 *
 * home_dir       working directory for mihomo (NULL: temporary directory)
 * config_yaml    base configuration (NULL: built-in defaults)
 * options_json   {"mixed_port":0,"log_level":"warning"} (NULL: defaults)
 * out_handle     receives the instance handle
 * out_error      receives an allocated error string on failure (may be NULL)
 *
 * Returns 0 on success, -1 on error. Free *out_error with milib_free_string().
 * Only one instance may be active per process.
 */
int milib_init(const char* home_dir,
               const char* config_yaml,
               const char* options_json,
               milib_handle* out_handle,
               char** out_error);

/* Stop the instance. Idempotent for the active handle. */
void milib_shutdown(milib_handle h);

/* Proxy management. Every function returns an allocated JSON string. */

/* Add one or more VLESS/VMess/Trojan/... links (or a base64 subscription). */
char* milib_add_proxy(milib_handle h, const char* link_or_blob, const char* options_json);

/* Remove a proxy added earlier (built-in proxies cannot be removed). */
char* milib_remove_proxy(milib_handle h, const char* name);

/* List the members of the MILIB group. */
char* milib_list_proxies(milib_handle h);

/* Select the active member of a selector group (NULL/empty group: MILIB). */
char* milib_select(milib_handle h, const char* group, const char* name);

/* Runtime counters, ports and versions. */
char* milib_status(milib_handle h);

/* SharedVPNLink API. */

/*
 * Create a link/proxy from a share link.
 *
 * options_json:
 *   {
 *     "name": "custom",
 *     "select": false,
 *     "local_proxy": true,
 *     "username": "milib",        // SOCKS username, generated default "milib"
 *     "password": "manual-pass",  // SOCKS password, random by default
 *     "proxy_password": "..."     // optional upstream credential override
 *   }
 *
 * Returns NULL on error; *out_error receives an allocated message.
 */
milib_link* milib_link_create(milib_handle h,
                              const char* link,
                              const char* options_json,
                              char** out_error);

/* Remove the proxy, close the local SOCKS endpoint and free the handle. */
int milib_link_destroy(milib_link* link);

/* Link description: id, name, type, server, port, socks addr + auth. */
char* milib_link_info(milib_link* link);

/* All live links. */
char* milib_link_list(milib_handle h);

/*
 * Ping: HTTP latency measured by sending traffic through the proxy.
 * options_json: {"url":"https://www.gstatic.com/generate_204",
 *                "timeout_ms":5000,"expected":"204"}
 */
char* milib_link_ping(milib_link* link, const char* options_json);

/*
 * Numeric ping: the delay in milliseconds is written to *out_delay_ms.
 *
 * Returns 0 on success, -1 on error. On error *out_error receives an allocated
 * message (free with milib_free_string).
 */
int milib_link_ping_ms(milib_link* link,
                       const char* options_json,
                       int* out_delay_ms,
                       char** out_error);

/*
 * Speed: download bytes through the proxy.
 * options_json: {"url":"https://...","bytes":5242880,"timeout_ms":15000}
 */
char* milib_link_speed(milib_link* link, const char* options_json);

/*
 * Exit IP: request an echo service through the proxy.
 * options_json: {"url":"https://api.ipify.org","format":"text","timeout_ms":10000}
 */
char* milib_link_exit_ip(milib_link* link, const char* options_json);

/* Async variants; results are delivered to the callback or polled by task id. */
int64_t milib_link_ping_async(milib_link* link,
                              const char* options_json,
                              milib_task_cb cb,
                              void* user);
int64_t milib_link_speed_async(milib_link* link,
                               const char* options_json,
                               milib_task_cb cb,
                               void* user);
int64_t milib_link_exit_ip_async(milib_link* link,
                                 const char* options_json,
                                 milib_task_cb cb,
                                 void* user);

/* Ping many links concurrently. links_json is a JSON array of ids: [1,2,3]. */
char* milib_links_ping_many(milib_handle h,
                            const char* links_json,
                            const char* options_json);

/* Async version of milib_links_ping_many. */
int64_t milib_links_ping_many_async(milib_handle h,
                                    const char* links_json,
                                    const char* options_json,
                                    milib_task_cb cb,
                                    void* user);

/* Latency tests ("ping"). */

/* Measure HTTP latency through an already added proxy. */
char* milib_test_delay(milib_handle h, const char* name, const char* options_json);

/* Measure HTTP latency of a share link without adding it. */
char* milib_test_link(milib_handle h, const char* link, const char* options_json);

/*
 * Schedule an asynchronous latency test. Returns a task id (>0) or -1.
 * The callback may be NULL; then poll with milib_task_poll().
 */
int64_t milib_test_delay_async(milib_handle h,
                               const char* name,
                               const char* options_json,
                               milib_task_cb cb,
                               void* user);

/* Poll an async task. Returns an allocated JSON string. */
char* milib_task_poll(milib_handle h, int64_t task_id);

/* Cancel a running task. Returns 0 on success. */
int milib_task_cancel(milib_handle h, int64_t task_id);

/* Cancel (if running) and forget a task. Returns 0 on success. */
int milib_task_free(milib_handle h, int64_t task_id);

/* Utilities. */

/* Library version. The pointer is static; do not free. */
const char* milib_version(void);

/* Free a string returned by milib_*. */
void milib_free_string(char* p);

#ifdef __cplusplus
}
#endif
#endif /* MILIB_H */
