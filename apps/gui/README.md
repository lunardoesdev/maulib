# mewnet GUI (Odin + raylib)

A simple GUI program for managing subscriptions and proxies from SQLite.

## Build and run

```bash
make gui
./build/mewnet-gui --db data/subs.db
```

For a check without a manual screenshot:

```bash
./build/mewnet-gui --db data/subs.db --screenshot /tmp/mewnet-gui.png
```

The GUI uses:

- `libmewnet` for all SQLite operations;
- `vendor:raylib` from Odin;
- only proxy pages, not the whole subscription;
- DejaVu Sans + Symbola fallback for Unicode/emoji.
  CJK characters that these fonts lack may render as `?`.

## What you can do

- view subscription groups; one group `All` by default;
- add a subscription to the selected group: URL + priority;
- delete the selected subscription;
- page through the group's proxies 16 at a time;
- see name, protocol, address, rating, alive/dead, latency, manual flag;
- select a proxy and view its full share link;
- add a manual proxy to the selected group;
- delete the selected proxy.

## Memory

The GUI **does not load**:

- the subscription body;
- all proxies of a subscription;
- all proxies of a group.

It keeps in memory:

- the list of groups and of subscriptions in the selected group (usually small);
- one page of `mewnet_subs_proxy_summary` without the full link;
- one selected full `mewnet_subs_proxy` record.

So a subscription with 10 000 configs uses roughly the same amount of memory as
one with 20.

## Library API for the GUI

Added to `libmewnet`:

- `mewnet_subs_load_groups` / `mewnet_subs_add_subscription_in_group`;
- `mewnet_subs_load_proxy_summaries` / `mewnet_subs_unload_proxy_summaries`
  — a page without the `link` field;
- `mewnet_subs_get_proxy` — one full record by id;
- `mewnet_subs_add_proxy` — a manual proxy into a subscription (`manual = 1`);
- `mewnet_subs_remove_proxy`;
- `mewnet_subs_count_proxies`.

Manual proxies are not removed on the next fetch if they are absent from the
subscription: generation-cleanup deletes only `manual = 0`.
