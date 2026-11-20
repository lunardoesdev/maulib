#ifndef MEWNET_H
#define MEWNET_H

#include "mewnet_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mewnet v2 - in-process mihomo core behind a small, raylib-style C ABI.
 *
 * Conventions:
 *   - options are value structs; use the matching *_default() function;
 *   - operations return values or result structs, not JSON;
 *   - lists are loaded with mewnet_load_*() and released with mewnet_unload_*();
 *   - handles are opaque pointers with an explicit destroy/close function;
 *   - no thread-local last-error state: errors are part of the result.
 */

int mewnet_abi_version(void);

/* Library version. The pointer is static; do not free. */
const char* mewnet_version(void);

/* Lifecycle. */
mewnet_config mewnet_config_default(void);
mewnet_init_result mewnet_init(const mewnet_config* config);
void mewnet_close(mewnet_instance* instance);

mewnet_status_info mewnet_get_status(mewnet_instance* instance);

/* Proxy management. */
mewnet_proxy_list mewnet_load_proxies(mewnet_instance* instance);
void mewnet_unload_proxies(mewnet_proxy_list list);

mewnet_add_options mewnet_add_options_default(void);
mewnet_error_code mewnet_add_proxy(mewnet_instance* instance,
                                 const char* uri_or_blob,
                                 const mewnet_add_options* options);
mewnet_error_code mewnet_remove_proxy(mewnet_instance* instance, const char* name);
mewnet_error_code mewnet_select_proxy(mewnet_instance* instance, const char* name);
/* True when a proxy with this name is registered in the running core. */
bool mewnet_has_proxy(mewnet_instance* instance, const char* name);

/* Latency tests for an already added proxy (by name) or a raw share link. */
mewnet_delay_options mewnet_delay_options_default(void);
mewnet_delay_result mewnet_test_delay(mewnet_instance* instance,
                                    const char* proxy_name,
                                    const mewnet_delay_options* options);
mewnet_delay_result mewnet_test_link(mewnet_instance* instance,
                                   const char* uri,
                                   const mewnet_delay_options* options);

/*
 * Stable identity of a share link: protocol + server + port + credentials.
 * Names, #fragments and transport-only options do not affect it, so the same
 * server/account from different subscriptions produces the same hash.
 * `hash` is the raw 32-byte SHA-256 digest.
 */
mewnet_fingerprint_result mewnet_link_fingerprint(const char* uri);

/*
 * GET a URL through the local mixed inbound (real client path).
 *
 * The result also carries the metrics stored by the agent: `ttfb_ms` (time to
 * the first byte; this is the ping), `ttlb_ms` (time to the last byte),
 * `bytes` (body bytes read, capped at 64 KiB) and `speed_bps` (body throughput).
 * `delay_ms` stays the total TTLB, as before.
 */
mewnet_probe_result mewnet_probe(mewnet_instance* instance,
                                 const char* url,
                                 int expected_status,
                                 int timeout_ms);

/*
 * Parse and test a raw share link, returning metadata and a detailed error.
 * The result carries the same TTFB/TTLB/bytes/speed metrics as mewnet_probe().
 */
mewnet_link_test_result mewnet_test_link_info(mewnet_instance* instance,
                                            const char* uri,
                                            const mewnet_delay_options* options);

/*
 * Test many links concurrently. `links` is an array of `count` C strings.
 * concurrency <= 0 means 1; it is capped at 256. The returned items array is
 * owned by the caller and released with mewnet_unload_batch_test().
 */
mewnet_batch_test_result mewnet_test_links_batch(mewnet_instance* instance,
                                                 const char* const* links,
                                                 size_t count,
                                                 const mewnet_delay_options* options,
                                                 size_t concurrency);
void mewnet_unload_batch_test(mewnet_batch_test_result result);

/*
 * Same as mewnet_test_links_batch(), but cancels the remaining checks as soon
 * as the first working link is found. Cancelled items have error CANCELLED and
 * must not be treated as tested.
 */
mewnet_batch_test_result mewnet_test_links_batch_first(mewnet_instance* instance,
                                                      const char* const* links,
                                                      size_t count,
                                                      const mewnet_delay_options* options,
                                                      size_t concurrency);

/* SharedVPNLink. */
mewnet_link_options mewnet_link_options_default(void);
mewnet_link_result mewnet_link_create(mewnet_instance* instance,
                                    const char* uri,
                                    const mewnet_link_options* options);
void mewnet_link_destroy(mewnet_link* link);

/*
 * Make this link the selected proxy of the MEWNET group, so traffic entering
 * the shared mixed inbound exits through this link. Equivalent to
 * mewnet_select_proxy(instance, name) with the link's own name, but takes the
 * handle you already have. The link's own SOCKS5 endpoint is unaffected.
 */
mewnet_error_code mewnet_link_select(mewnet_link* link);

mewnet_link_info mewnet_link_get_info(mewnet_link* link);

mewnet_link_list mewnet_load_links(mewnet_instance* instance);
void mewnet_unload_links(mewnet_link_list list);

/* Per-link tests. */
mewnet_ping_result mewnet_link_ping(mewnet_link* link,
                                  const mewnet_delay_options* options);

mewnet_speed_options mewnet_speed_options_default(void);
mewnet_speed_result mewnet_link_speed(mewnet_link* link,
                                    const mewnet_speed_options* options);

mewnet_exit_ip_options mewnet_exit_ip_options_default(void);
mewnet_exit_ip_result mewnet_link_exit_ip(mewnet_link* link,
                                        const mewnet_exit_ip_options* options);

mewnet_ping_many_result mewnet_link_ping_many(mewnet_instance* instance,
                                            mewnet_link* const* links,
                                            size_t count,
                                            const mewnet_delay_options* options);
void mewnet_unload_ping_many(mewnet_ping_many_result result);

/*
 * Async tasks.
 *
 * The callback runs on an arbitrary OS thread. Do not call mewnet_close,
 * mewnet_link_destroy, or mewnet_task_destroy from the callback. Result pointers
 * remain valid until mewnet_task_destroy(). A task created with cb == NULL can
 * be polled with mewnet_task_poll().
 */
mewnet_task* mewnet_test_delay_async(mewnet_instance* instance,
                                   const char* proxy_name,
                                   const mewnet_delay_options* options,
                                   mewnet_task_cb cb,
                                   void* user);
mewnet_task* mewnet_link_ping_async(mewnet_link* link,
                                  const mewnet_delay_options* options,
                                  mewnet_task_cb cb,
                                  void* user);
mewnet_task* mewnet_link_speed_async(mewnet_link* link,
                                   const mewnet_speed_options* options,
                                   mewnet_task_cb cb,
                                   void* user);
mewnet_task* mewnet_link_exit_ip_async(mewnet_link* link,
                                     const mewnet_exit_ip_options* options,
                                     mewnet_task_cb cb,
                                     void* user);
mewnet_task* mewnet_link_ping_many_async(mewnet_instance* instance,
                                       mewnet_link* const* links,
                                       size_t count,
                                       const mewnet_delay_options* options,
                                       mewnet_task_cb cb,
                                       void* user);

/*
 * Poll a task. For MEWNET_TASK_PING_MANY, result.pings points at task-owned
 * storage; it is valid until the next poll or mewnet_task_destroy().
 */
mewnet_task_result mewnet_task_poll(mewnet_task* task);
mewnet_error_code mewnet_task_cancel(mewnet_task* task);
void mewnet_task_destroy(mewnet_task* task);

/* Streaming HTTP. No instance is required. */
mewnet_http_result mewnet_http_stream(const char* url,
                                    const mewnet_http_options* options,
                                    mewnet_http_cb cb,
                                    void* user);

/* Logging. */
void mewnet_set_log_level(mewnet_log_level level);
void mewnet_set_log_callback(mewnet_log_cb cb, void* user);

#ifdef __cplusplus
}
#endif
#endif /* MEWNET_H */
