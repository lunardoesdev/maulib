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
    milib_handle handle = 0;
    char *error = NULL;

    expect(strcmp(milib_version(), "0.1.0") == 0, "version", milib_version());

    int rc = milib_init(NULL, NULL, "{\"mixed_port\":0}", &handle, &error);
    if (rc != 0) {
        printf("FAIL  init  %s\n", error ? error : "unknown");
        milib_free_string(error);
        return 1;
    }
    expect(rc == 0 && handle != 0, "init", "");

    char *status = milib_status(handle);
    expect(status && strstr(status, "\"ok\":true"), "status ok", status);
    milib_free_string(status);

    const char *link =
        "vless://11111111-2222-3333-4444-555555555555@127.0.0.1:1"
        "?encryption=none&security=none&type=tcp#c-dead-node";
    char *added = milib_add_proxy(handle, link, NULL);
    expect(added && strstr(added, "\"ok\":true") && strstr(added, "c-dead-node"),
           "add proxy", added);
    milib_free_string(added);

    char *listed = milib_list_proxies(handle);
    expect(listed && strstr(listed, "c-dead-node"), "list proxies", listed);
    milib_free_string(listed);

    char *selected = milib_select(handle, NULL, "DIRECT");
    expect(selected && strstr(selected, "\"ok\":true"), "select DIRECT", selected);
    milib_free_string(selected);

    char *removed = milib_remove_proxy(handle, "c-dead-node");
    expect(removed && strstr(removed, "\"ok\":true"), "remove proxy", removed);
    milib_free_string(removed);

    char *missing = milib_remove_proxy(handle, "nope");
    expect(missing && strstr(missing, "\"ok\":false"), "remove missing fails", missing);
    milib_free_string(missing);

    /* SharedVPNLink API. */
    char *link_error = NULL;
    milib_link *vpn = milib_link_create(handle, link, NULL, &link_error);
    expect(vpn != NULL, "link create", link_error ? link_error : "");
    if (link_error) {
        milib_free_string(link_error);
    }
    if (vpn != NULL) {
        char *info = milib_link_info(vpn);
        expect(info && strstr(info, "\"ok\":true") && strstr(info, "socks"),
               "link info", info);
        milib_free_string(info);

        char *links = milib_link_list(handle);
        expect(links && strstr(links, "\"count\":1"), "link list", links);
        milib_free_string(links);

        expect(milib_link_destroy(vpn) == 0, "link destroy", "");

        char *after = milib_link_list(handle);
        expect(after && strstr(after, "\"count\":0"), "link list after destroy", after);
        milib_free_string(after);
    }

    milib_shutdown(handle);
    printf("\n%s\n", failures == 0 ? "all C tests passed" : "C tests failed");
    return failures == 0 ? 0 : 1;
}
