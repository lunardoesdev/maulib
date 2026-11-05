/*
 * Minimal C ABI smoke test for libmilib.so.
 *
 * Build (see Makefile):
 *   gcc tests/c/test_api.c -Iinclude -Ldist -lmilib \
 *       -Wl,-rpath,'$ORIGIN' -o dist/test_api
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "milib.h"

static int failures = 0;

static void expect(int condition, const char *what, const char *detail) {
    if (condition) {
        printf("PASS  %s\n", what);
    } else {
        printf("FAIL  %s  %s\n", what, detail ? detail : "");
        failures++;
    }
}

int main(void) {
    expect(milib_abi_version() == MILIB_ABI_VERSION, "abi version", "");
    expect(strcmp(milib_version(), "0.2.0") == 0, "version", milib_version());

    milib_config config = milib_config_default();
    milib_init_result init = milib_init(&config);
    expect(init.instance != NULL && init.error.code == MILIB_ERROR_NONE,
           "init", init.error.message);
    if (init.instance == NULL) {
        return 1;
    }
    milib_instance *instance = init.instance;

    milib_status_info status = milib_get_status(instance);
    expect(status.ok && status.running, "status ok", "");

    const char *link =
        "vless://11111111-2222-3333-4444-555555555555@127.0.0.1:1"
        "?encryption=none&security=none&type=tcp#c-dead-node";

    milib_add_options add_options = milib_add_options_default();
    milib_error_code rc = milib_add_proxy(instance, link, &add_options);
    expect(rc == MILIB_ERROR_NONE, "add proxy", "");

    milib_proxy_list proxies = milib_load_proxies(instance);
    int found = 0;
    for (size_t i = 0; i < proxies.count; i++) {
        if (strcmp(proxies.items[i].name, "c-dead-node") == 0) {
            found = 1;
            break;
        }
    }
    expect(proxies.ok && found, "load proxies", "");
    expect(strcmp(proxies.selected, "c-dead-node") == 0,
           "added proxy is selected", proxies.selected);
    milib_unload_proxies(proxies);

    expect(milib_select_proxy(instance, "DIRECT") == MILIB_ERROR_NONE,
           "select DIRECT", "");

    expect(milib_remove_proxy(instance, "c-dead-node") == MILIB_ERROR_NONE,
           "remove proxy", "");
    expect(milib_remove_proxy(instance, "nope") != MILIB_ERROR_NONE,
           "remove missing fails", "");

    milib_link_options link_options = milib_link_options_default();
    milib_link_result created = milib_link_create(instance, link, &link_options);
    expect(created.link != NULL && created.error.code == MILIB_ERROR_NONE,
           "link create", created.error.message);
    if (created.link != NULL) {
        milib_link *vpn = created.link;

        milib_link_info info = milib_link_get_info(vpn);
        expect(info.ok && info.id > 0 && strcmp(info.name, "c-dead-node") == 0 &&
                   info.socks_addr[0] != '\0',
               "link info", "");
        expect(strcmp(info.socks_username, "milib") == 0 &&
                   strlen(info.socks_password) >= 16,
               "link info has auth", info.socks_password);

        milib_link_list links = milib_load_links(instance);
        expect(links.ok && links.count == 1 && links.items[0].id == info.id,
               "load links", "");
        milib_unload_links(links);

        milib_link_destroy(vpn);

        links = milib_load_links(instance);
        expect(links.ok && links.count == 0, "load links after destroy", "");
        milib_unload_links(links);
    }

    milib_close(instance);
    printf("\n%s\n", failures == 0 ? "all C tests passed" : "C tests failed");
    return failures == 0 ? 0 : 1;
}
