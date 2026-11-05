#ifndef MILIB_H
#define MILIB_H

#include "milib_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * milib v2 - in-process mihomo core behind a small, raylib-style C ABI.
 *
 * Conventions:
 *   - options are value structs; use the matching *_default() function;
 *   - operations return values or result structs, not JSON;
 *   - lists are loaded with milib_load_*() and released with milib_unload_*();
 *   - handles are opaque pointers with an explicit destroy/close function;
 *   - no thread-local last-error state: errors are part of the result.
 */

int milib_abi_version(void);

/* Library version. The pointer is static; do not free. */
const char* milib_version(void);

/* Lifecycle. */
milib_config milib_config_default(void);
milib_init_result milib_init(const milib_config* config);
void milib_close(milib_instance* instance);

milib_status_info milib_get_status(milib_instance* instance);

/* Proxy management. */
milib_proxy_list milib_load_proxies(milib_instance* instance);
void milib_unload_proxies(milib_proxy_list list);

milib_add_options milib_add_options_default(void);
milib_error_code milib_add_proxy(milib_instance* instance,
                                 const char* uri_or_blob,
                                 const milib_add_options* options);
milib_error_code milib_remove_proxy(milib_instance* instance, const char* name);
milib_error_code milib_select_proxy(milib_instance* instance, const char* name);

/* Latency tests for an already added proxy (by name) or a raw share link. */
milib_delay_options milib_delay_options_default(void);
milib_delay_result milib_test_delay(milib_instance* instance,
                                    const char* proxy_name,
                                    const milib_delay_options* options);
milib_delay_result milib_test_link(milib_instance* instance,
                                   const char* uri,
                                   const milib_delay_options* options);

/* SharedVPNLink. */
milib_link_options milib_link_options_default(void);
milib_link_result milib_link_create(milib_instance* instance,
                                    const char* uri,
                                    const milib_link_options* options);
void milib_link_destroy(milib_link* link);
milib_link_info milib_link_get_info(milib_link* link);

milib_link_list milib_load_links(milib_instance* instance);
void milib_unload_links(milib_link_list list);

/* Per-link tests. */
milib_ping_result milib_link_ping(milib_link* link,
                                  const milib_delay_options* options);

milib_speed_options milib_speed_options_default(void);
milib_speed_result milib_link_speed(milib_link* link,
                                    const milib_speed_options* options);

milib_exit_ip_options milib_exit_ip_options_default(void);
milib_exit_ip_result milib_link_exit_ip(milib_link* link,
                                        const milib_exit_ip_options* options);

milib_ping_many_result milib_link_ping_many(milib_instance* instance,
                                            milib_link* const* links,
                                            size_t count,
                                            const milib_delay_options* options);
void milib_unload_ping_many(milib_ping_many_result result);

/*
 * Async tasks.
 *
 * The callback runs on an arbitrary OS thread. Do not call milib_close,
 * milib_link_destroy, or milib_task_destroy from the callback. Result pointers
 * remain valid until milib_task_destroy(). A task created with cb == NULL can
 * be polled with milib_task_poll().
 */
milib_task* milib_test_delay_async(milib_instance* instance,
                                   const char* proxy_name,
                                   const milib_delay_options* options,
                                   milib_task_cb cb,
                                   void* user);
milib_task* milib_link_ping_async(milib_link* link,
                                  const milib_delay_options* options,
                                  milib_task_cb cb,
                                  void* user);
milib_task* milib_link_speed_async(milib_link* link,
                                   const milib_speed_options* options,
                                   milib_task_cb cb,
                                   void* user);
milib_task* milib_link_exit_ip_async(milib_link* link,
                                     const milib_exit_ip_options* options,
                                     milib_task_cb cb,
                                     void* user);
milib_task* milib_link_ping_many_async(milib_instance* instance,
                                       milib_link* const* links,
                                       size_t count,
                                       const milib_delay_options* options,
                                       milib_task_cb cb,
                                       void* user);

/*
 * Poll a task. For MILIB_TASK_PING_MANY, result.pings points at task-owned
 * storage; it is valid until the next poll or milib_task_destroy().
 */
milib_task_result milib_task_poll(milib_task* task);
milib_error_code milib_task_cancel(milib_task* task);
void milib_task_destroy(milib_task* task);

/* Logging. */
void milib_set_log_level(milib_log_level level);
void milib_set_log_callback(milib_log_cb cb, void* user);

#ifdef __cplusplus
}
#endif
#endif /* MILIB_H */
