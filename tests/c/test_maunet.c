/*
 * C ABI smoke test for the maunet facade: global init, link ids, default
 * selection, subscription storage and teardown.
 */

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "maunet.h"

static int failures = 0;

static void expect(int condition, const char* what, const char* detail) {
    if (condition) {
        printf("PASS  %s\n", what);
    } else {
        printf("FAIL  %s  %s\n", what, detail != NULL ? detail : "");
        failures++;
    }
}

int main(void) {
    char db_path[256];
    snprintf(db_path, sizeof(db_path), "/tmp/maunet-test-%ld.db", (long)getpid());
    unlink(db_path);

    /* A second init must fail cleanly, before the real one. */
    maunet_settings settings = MaunetSettingsDefault();
    settings.db_path = db_path;
    settings.mixed_port = 0;
    settings.request_limit = 0;
    settings.test_timeout_ms = 800;
    settings.test_url = "http://127.0.0.1:9/204";

    /* Slow-node policy defaults: the agent replaces an active link that is 3x
     * slower than the pool, above 1 s, three checks in a row. */
    expect(settings.slow_factor == 3 && settings.slow_floor_ms == 1000 &&
               settings.slow_checks == 3 && settings.slow_cooldown_sec == 600,
           "slow policy defaults", "");

    expect(!IsMaunetReady(), "not ready before init", "");
    expect(!SetMaunetLinkAsDefault(1), "link call before init fails", "");
    expect(GetMaunetError()[0] != '\0', "error message before init", "");

    expect(InitMaunet(&settings), "init", GetMaunetError());
    expect(IsMaunetReady(), "ready after init", "");
    expect(GetMaunetProxyPort() > 0, "local proxy port", "");
    expect(GetMaunetError()[0] == '\0', "error cleared after init", GetMaunetError());

    expect(!InitMaunet(&settings), "second init fails", "");
    expect(strstr(GetMaunetError(), "already") != NULL, "second init explains why",
           GetMaunetError());

    const char* dead_a =
        "vless://11111111-2222-3333-4444-555555555555@127.0.0.1:1"
        "?encryption=none&security=none&type=tcp#maunet-a";
    const char* dead_b =
        "vless://66666666-7777-8888-9999-000000000000@127.0.0.1:2"
        "?encryption=none&security=none&type=tcp#maunet-b";

    maunet_link_result added = AddMaunetLink(dead_a);
    expect(added.id > 0 && added.error.code == MEWNET_ERROR_NONE, "add link a",
           added.error.message);
    int64_t link_a = added.id;

    /* Duplicates never create a second row: the caller gets the existing id. */
    maunet_link_result again = AddMaunetLink(dead_a);
    expect(again.id == link_a, "same link returns the same id", "");
    expect(again.error.code == MEWNET_ERROR_NONE, "duplicate is not an error",
           again.error.message);
    expect(GetMaunetLinkCount() == 1, "duplicate does not add a row", "");

    const char* same_identity =
        "vless://11111111-2222-3333-4444-555555555555@127.0.0.1:1"
        "?encryption=none&security=none&type=tcp&remarked=1#maunet-a-renamed";
    maunet_link_result renamed = AddMaunetLink(same_identity);
    expect(renamed.id == link_a, "same identity under another name keeps the id",
           "");
    expect(GetMaunetLinkCount() == 1, "identity duplicate does not add a row", "");

    expect(GetMaunetLinkCount() == 1, "link count is one", "");
    expect(!GetMaunetLink(999999).id, "missing link returns empty info", "");

    maunet_link_info info = GetMaunetLink(link_a);
    expect(info.id == link_a, "get link by id", "");
    expect(strcmp(info.name, "maunet-a") == 0, "link name from the fragment", info.name);
    expect(strcasecmp(info.protocol, "vless") == 0, "link protocol", info.protocol);
    expect(info.port == 1, "link port", "");
    expect(info.manual, "link is manual", "");
    expect(!info.in_core, "link is not in the core before selection", "");

    expect(SetMaunetLinkAsDefault(link_a), "set default", GetMaunetError());
    expect(GetMaunetDefaultLink() == link_a, "default link id", "");
    expect(GetMaunetLink(link_a).in_core, "default link is in the core", "");
    expect(GetMaunetLink(link_a).is_default, "link reports default", "");

    maunet_status status = GetMaunetStatus();
    expect(status.ready, "status ready", "");
    expect(status.links == 1 && status.subscriptions == 0, "status counters", "");
    expect(status.default_link == link_a, "status default link", "");
    expect(strcmp(status.selected, "maunet-a") == 0, "status selected proxy",
           status.selected);
    expect(status.proxy_port > 0, "status proxy port", "");

    expect(!TestMaunetLink(link_a), "dead link fails the test", "");
    expect(GetMaunetError()[0] != '\0', "test failure has a message",
           GetMaunetError());

    maunet_link_list links = LoadMaunetLinks(10, 0);
    expect(links.ok && links.count == 1, "load links", "");
    if (links.ok && links.count == 1) {
        expect(links.items[0].is_default, "loaded link is default", "");
    }
    UnloadMaunetLinks(links);

    maunet_link_result second = AddMaunetLink(dead_b);
    expect(second.id > 0, "add link b", second.error.message);
    expect(SetMaunetLinkAsDefault(second.id), "switch default to b", "");
    expect(GetMaunetDefaultLink() == second.id, "default moved to b", "");
    expect(GetMaunetLink(link_a).in_core, "previous link stays in the core", "");
    expect(!GetMaunetLink(link_a).is_default, "previous link is no longer default", "");

    expect(RemoveMaunetLink(second.id), "remove default link", GetMaunetError());
    expect(GetMaunetDefaultLink() == 0, "default cleared with the link", "");
    expect(GetMaunetLinkCount() == 1, "one link left", "");

    /* Subscriptions: the fetch fails (nothing listens on port 9), the row stays. */
    maunet_link_result sub = AddMaunetSubscription("http://127.0.0.1:9/sub.txt");
    expect(sub.id > 0, "add subscription keeps the row", sub.error.message);
    expect(GetMaunetSubscriptionCount() == 1, "subscription count", "");

    maunet_subscription_list subs = LoadMaunetSubscriptions();
    expect(subs.ok && subs.count == 1, "load subscriptions", "");
    if (subs.ok && subs.count == 1) {
        expect(strcmp(subs.items[0].url, "http://127.0.0.1:9/sub.txt") == 0,
               "subscription url", subs.items[0].url);
        expect(subs.items[0].failure_count >= 0, "subscription failure count", "");
    }
    UnloadMaunetSubscriptions(subs);

    expect(!RefreshMaunetSubscriptions(), "refresh reports total failure", "");
    expect(strstr(GetMaunetError(), "failed") != NULL, "refresh error message",
           GetMaunetError());

    expect(RemoveMaunetSubscription(sub.id), "remove subscription", GetMaunetError());
    expect(GetMaunetSubscriptionCount() == 0, "subscriptions cleared", "");

    expect(RemoveMaunetLink(link_a), "remove last link", GetMaunetError());
    expect(GetMaunetLinkCount() == 0, "links cleared", "");

    DeinitMaunet();
    expect(!IsMaunetReady(), "not ready after deinit", "");
    DeinitMaunet(); /* idempotent */
    expect(!AddMaunetLink(dead_a).id, "add after deinit fails", "");

    /* The agent and its slow-node policy are reachable through the settings
     * alone: a negative slow_factor turns the policy off. There are no
     * subscriptions, so the agent starts and idles without network traffic. */
    char agent_db[256];
    snprintf(agent_db, sizeof(agent_db), "/tmp/maunet-agent-%ld.db", (long)getpid());
    unlink(agent_db);
    maunet_settings agent_settings = MaunetSettingsDefault();
    agent_settings.db_path = agent_db;
    agent_settings.request_limit = 0;
    agent_settings.run_agent = true;
    agent_settings.slow_factor = -1;
    agent_settings.slow_floor_ms = 1500;
    agent_settings.slow_checks = 5;
    agent_settings.slow_cooldown_sec = 120;
    expect(InitMaunet(&agent_settings), "init with the agent enabled", GetMaunetError());
    expect(GetMaunetStatus().agent_running, "agent started from the settings", "");
    DeinitMaunet();
    expect(!IsMaunetReady(), "deinit stops the agent", "");
    unlink(agent_db);

    unlink(db_path);
    printf("\n%s\n", failures == 0 ? "all maunet tests passed" : "maunet tests failed");
    return failures == 0 ? 0 : 1;
}
