# mewnet-watch

A utility for initial discovery and routing through the discovered proxy.

```bash
./build/apps/mewnet-watch/mewnet-watch \
  --db data/watch.db \
  --proxy-port 18085 \
  --batch 70 --concurrency 70 --timeout 3000 \
  https://raw.githubusercontent.com/Pawdroid/Free-servers/main/sub \
  https://raw.githubusercontent.com/ALIILAPRO/v2rayNG-Config/main/server.txt
```

What it does:

- creates/opens an SQLite database;
- adds the passed subscriptions to the `All` group;
- while no working node exists, checks untested proxies in batches
  (`--batch`, 70 by default) concurrently (`--concurrency`, 70 by default);
- candidates are selected randomly with weights: the higher the group/subscription
  priority and the rating, the higher the chance of landing in a batch;
- stops the remaining checks in the batch as soon as the first working
  node is found;
- adds it to `MILIB`, selects it as active, and prints:
  ```text
  PROXY http://127.0.0.1:<port>
  READY
  ```
- after that continues discovery through the standard rate limiter;
- refreshes at most one subscription per hour (`--fetch-interval 3600`);
- if there are no untested nodes and none working were found — refreshes the next
  subscription immediately, without waiting an hour.

Checking through the discovered proxy:

```bash
curl -x http://127.0.0.1:18085 https://lua.org
```

`mewnet-watch` uses only the public `libmewnet` API and `libmewnet`.
