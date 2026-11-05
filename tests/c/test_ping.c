/*
 * Ping a proxy through the C ABI and print the delay as a number.
 *
 * Usage:
 *   ./test_ping 'vless://...' [url]
 *   ./test_ping --json 'vless://...' [url]
 *
 * or:
 *   MILIB_TEST_VLESS='vless://...' ./test_ping
 *
 * Default output is a single integer: delay in milliseconds. Exit code 0 means
 * the ping succeeded. The ping is an HTTP request sent through the proxy, not
 * an ICMP ping to the proxy server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "milib.h"

int main(int argc, char **argv) {
    int json_mode = 0;
    int argi = 1;

    if (argi < argc && strcmp(argv[argi], "--json") == 0) {
        json_mode = 1;
        argi++;
    }

    const char *link = argi < argc ? argv[argi++] : getenv("MILIB_TEST_VLESS");
    const char *url = argi < argc ? argv[argi] : getenv("MILIB_PING_URL");

    if (url == NULL || url[0] == '\0') {
        url = "https://www.gstatic.com/generate_204";
    }
    if (link == NULL || link[0] == '\0') {
        fprintf(stderr,
                "usage: %s [--json] 'vless://...' [url]\n"
                "   or: MILIB_TEST_VLESS='vless://...' %s\n",
                argv[0], argv[0]);
        return 2;
    }

    milib_handle handle = 0;
    char *error = NULL;

    /* mixed_port: -1 disables the inbound; ping does not need it. */
    if (milib_init(NULL, NULL, "{\"mixed_port\":-1,\"log_level\":\"silent\"}",
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

    char options[1024];
    snprintf(options, sizeof(options),
             "{\"url\":\"%s\",\"timeout_ms\":5000,\"expected\":\"204\"}", url);

    if (json_mode) {
        char *info = milib_link_info(vpn);
        printf("link: %s\n", info ? info : "(null)");
        milib_free_string(info);

        char *result = milib_link_ping(vpn, options);
        printf("ping: %s\n", result ? result : "(null)");
        milib_free_string(result);
    } else {
        int delay_ms = 0;
        if (milib_link_ping_ms(vpn, options, &delay_ms, &error) != 0) {
            fprintf(stderr, "ping failed: %s\n", error ? error : "unknown");
            milib_free_string(error);
            milib_link_destroy(vpn);
            milib_shutdown(handle);
            return 1;
        }
        printf("%d\n", delay_ms);
    }

    milib_link_destroy(vpn);
    milib_shutdown(handle);
    return 0;
}
