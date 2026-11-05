/*
 * Ping a proxy through the C ABI and print the delay as a number.
 *
 * Usage:
 *   ./test_ping 'vless://...' [url]
 *   ./test_ping --verbose 'vless://...' [url]
 *
 * or:
 *   MEWNET_TEST_VLESS='vless://...' ./test_ping
 *
 * Default output is a single integer: delay in milliseconds. Exit code 0 means
 * the ping succeeded. The ping is an HTTP request sent through the proxy, not
 * an ICMP ping to the proxy server.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mewnet.h"

static void on_log(mewnet_log_level level, const char *message, void *user) {
    (void)user;
    if (level >= MEWNET_LOG_WARNING) {
        fprintf(stderr, "mewnet: %s\n", message);
    }
}

int main(int argc, char **argv) {
    int verbose = 0;
    int argi = 1;

    if (argi < argc &&
        (strcmp(argv[argi], "--verbose") == 0 || strcmp(argv[argi], "-v") == 0)) {
        verbose = 1;
        argi++;
    }

    const char *link = argi < argc ? argv[argi++] : getenv("MEWNET_TEST_VLESS");
    const char *url = argi < argc ? argv[argi] : getenv("MEWNET_PING_URL");

    if (url == NULL || url[0] == '\0') {
        url = "https://www.gstatic.com/generate_204";
    }
    if (link == NULL || link[0] == '\0') {
        fprintf(stderr,
                "usage: %s [--verbose] 'vless://...' [url]\n"
                "   or: MEWNET_TEST_VLESS='vless://...' %s\n",
                argv[0], argv[0]);
        return 2;
    }

    mewnet_config config = mewnet_config_default();
    config.mixed_port = -1;
    config.log_level = "error";

    mewnet_init_result init = mewnet_init(&config);
    if (init.instance == NULL) {
        fprintf(stderr, "init failed: %s\n", init.error.message);
        return 1;
    }
    mewnet_instance *instance = init.instance;
    mewnet_set_log_callback(on_log, NULL);

    mewnet_link_options link_options = mewnet_link_options_default();
    link_options.local_proxy = false;

    mewnet_link_result created = mewnet_link_create(instance, link, &link_options);
    if (created.link == NULL) {
        fprintf(stderr, "link_create failed: %s\n", created.error.message);
        mewnet_close(instance);
        return 1;
    }
    mewnet_link *vpn = created.link;

    mewnet_delay_options delay_options = mewnet_delay_options_default();
    delay_options.url = url;
    delay_options.timeout_ms = 5000;
    delay_options.expected = "204";

    if (verbose) {
        mewnet_link_info info = mewnet_link_get_info(vpn);
        if (info.ok) {
            printf("link: id=%llu name=%s type=%s server=%s port=%d\n",
                   (unsigned long long)info.id, info.name, info.proxy_type,
                   info.server, info.port);
        } else {
            fprintf(stderr, "link info failed: code %d\n", info.error);
        }
    }

    mewnet_ping_result result = mewnet_link_ping(vpn, &delay_options);
    int exit_code = 0;
    if (!result.ok) {
        fprintf(stderr, "ping failed: code %d\n", result.error);
        exit_code = 1;
    } else {
        printf("%u\n", result.delay_ms);
    }

    mewnet_link_destroy(vpn);
    mewnet_close(instance);
    return exit_code;
}
