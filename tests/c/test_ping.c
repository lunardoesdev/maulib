/*
 * Ping a proxy through the C ABI.
 *
 * Usage:
 *   ./test_ping 'vless://...' [url]
 *
 * or:
 *   MILIB_TEST_VLESS='vless://...' MILIB_PING_URL='http://127.0.0.1:8080/204' ./test_ping
 *
 * Exit code 0 means the ping succeeded (delay > 0). The ping is an HTTP
 * request sent through the proxy, not an ICMP ping to the proxy server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "milib.h"

int main(int argc, char **argv) {
    const char *link = argc > 1 ? argv[1] : getenv("MILIB_TEST_VLESS");
    const char *url = argc > 2 ? argv[2] : getenv("MILIB_PING_URL");

    if (url == NULL || url[0] == '\0') {
        url = "https://www.gstatic.com/generate_204";
    }
    if (link == NULL || link[0] == '\0') {
        fprintf(stderr,
                "usage: %s 'vless://...' [url]\n"
                "   or: MILIB_TEST_VLESS='vless://...' %s\n",
                argv[0], argv[0]);
        return 2;
    }

    milib_handle handle = 0;
    char *error = NULL;

    /* mixed_port: -1 disables the inbound; ping does not need it. */
    if (milib_init(NULL, NULL, "{\"mixed_port\":-1,\"log_level\":\"error\"}",
                   &handle, &error) != 0) {
        fprintf(stderr, "init failed: %s\n", error ? error : "unknown");
        milib_free_string(error);
        return 1;
    }

    milib_link *vpn = milib_link_create(handle, link, "{\"local_proxy\":false}", &error);
    if (vpn == NULL) {
        fprintf(stderr, "link_create failed: %s\n", error ? error : "unknown");
        milib_free_string(error);
        milib_shutdown(handle);
        return 1;
    }

    char *info = milib_link_info(vpn);
    printf("link: %s\n", info ? info : "(null)");
    milib_free_string(info);

    char options[1024];
    snprintf(options, sizeof(options),
             "{\"url\":\"%s\",\"timeout_ms\":5000,\"expected\":\"204\"}", url);

    char *result = milib_link_ping(vpn, options);
    printf("ping: %s\n", result ? result : "(null)");

    int ok = result != NULL && strstr(result, "\"ok\":true") != NULL;

    milib_free_string(result);
    milib_link_destroy(vpn);
    milib_shutdown(handle);
    return ok ? 0 : 1;
}
