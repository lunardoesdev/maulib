/*
 * Minimal C ABI smoke test for libmewnet.so.
 *
 * Build (see Makefile):
 *   gcc tests/c/test_api.c -Iinclude -Ldist -lmewnet \
 *       -Wl,-rpath,'$ORIGIN' -o dist/test_api
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mewnet.h"

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
    expect(mewnet_abi_version() == MEWNET_ABI_VERSION, "abi version", "");
    expect(strcmp(mewnet_version(), "0.2.0") == 0, "version", mewnet_version());

    mewnet_config config = mewnet_config_default();
    mewnet_init_result init = mewnet_init(&config);
    expect(init.instance != NULL && init.error.code == MEWNET_ERROR_NONE,
           "init", init.error.message);
    if (init.instance == NULL) {
        return 1;
    }
    mewnet_instance *instance = init.instance;

    mewnet_status_info status = mewnet_get_status(instance);
    expect(status.ok && status.running, "status ok", "");

    const char *link =
        "vless://11111111-2222-3333-4444-555555555555@127.0.0.1:1"
        "?encryption=none&security=none&type=tcp#c-dead-node";

    mewnet_add_options add_options = mewnet_add_options_default();
    mewnet_error_code rc = mewnet_add_proxy(instance, link, &add_options);
    expect(rc == MEWNET_ERROR_NONE, "add proxy", "");

    mewnet_proxy_list proxies = mewnet_load_proxies(instance);
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
    mewnet_unload_proxies(proxies);

    expect(mewnet_select_proxy(instance, "DIRECT") == MEWNET_ERROR_NONE,
           "select DIRECT", "");

    expect(mewnet_remove_proxy(instance, "c-dead-node") == MEWNET_ERROR_NONE,
           "remove proxy", "");
    expect(mewnet_remove_proxy(instance, "nope") != MEWNET_ERROR_NONE,
           "remove missing fails", "");

    mewnet_link_options link_options = mewnet_link_options_default();
    mewnet_link_result created = mewnet_link_create(instance, link, &link_options);
    expect(created.link != NULL && created.error.code == MEWNET_ERROR_NONE,
           "link create", created.error.message);
    if (created.link != NULL) {
        mewnet_link *vpn = created.link;

        mewnet_link_info info = mewnet_link_get_info(vpn);
        expect(info.ok && info.id > 0 && strcmp(info.name, "c-dead-node") == 0 &&
                   info.socks_addr[0] != '\0',
               "link info", "");
        expect(strcmp(info.socks_username, "mewnet") == 0 &&
                   strlen(info.socks_password) >= 16,
               "link info has auth", info.socks_password);

        mewnet_link_list links = mewnet_load_links(instance);
        expect(links.ok && links.count == 1 && links.items[0].id == info.id,
               "load links", "");
        mewnet_unload_links(links);

        expect(!info.selected, "fresh link is not selected", "");
        expect(mewnet_link_select(vpn) == MEWNET_ERROR_NONE, "link select", "");
        info = mewnet_link_get_info(vpn);
        expect(info.ok && info.selected, "selected link reports selected", "");
        expect(strcmp(mewnet_get_status(instance).selected, info.name) == 0,
               "link select sets the main proxy", "");

        mewnet_link_destroy(vpn);

        links = mewnet_load_links(instance);
        expect(links.ok && links.count == 0, "load links after destroy", "");
        mewnet_unload_links(links);
    }

    mewnet_close(instance);
    printf("\n%s\n", failures == 0 ? "all C tests passed" : "C tests failed");
    return failures == 0 ? 0 : 1;
}
