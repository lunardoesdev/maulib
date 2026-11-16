# mewnet-discovery

A probe for the discovery pipeline, not a proxy client. It fetches
subscriptions, then grows the pool of working proxies in three stages and
prints how long each stage took, so a change in the agent or the test policy can
be measured instead of guessed.

## Stages

| Stage | Behaviour | Question it answers |
|---|---|---|
| 1 fast | wide batches (default 64 checks at 64 parallel) until `--fast` nodes work | how quickly does the first usable node appear? |
| 2 bulk | smaller paced batches (16 checks, 8 parallel, 250 ms pause) until `--bulk` nodes work | how long does a real pool of a hundred take? |
| 3 trickle | one small batch every `--trickle-every` seconds, refreshing subscriptions when due | what does the slow background pass add over time? |

Stage 1 stops as soon as the target is reached, so the reported number is the
time to *first usable pool*, not the time to test everything.

## Run

```bash
make subs        # builds all clients, including this one
./build/apps/mewnet-discovery/mewnet-discovery --db data/discovery.db \
  --fast 8 --bulk 100 --run-seconds 300 \
  https://raw.githubusercontent.com/mahdibland/V2RayAggregator/master/sub/sub_merge.txt \
  https://raw.githubusercontent.com/barry-far/V2ray-Config/main/All_Configs_Sub.txt
```

or the shortcut:

```bash
make discovery   # same, 120 s, one subscription
```

Sample output from a run on two public subscriptions (4314 configs):

```text
== fetching 2 subscription(s)
  fetch HTTP 200: +4303 ~249 -0 (mahdibland/V2RayAggregator)
  fetch HTTP 200: +4 ~15 -0 (Pawdroid/Free-servers)
  configs in database: 4314
== stage 1: first 5 working proxies (batch 96, concurrency 96)
    1.1s tested=57 alive=1 untested=4257
    3.0s tested=105 alive=2 untested=4209
    7.6s tested=204 alive=5 untested=4110
== stage 1 done: 5 alive in 7.9 s (first node after 1.9 s)
== stage 2: grow to 12 proxies (batch 24, concurrency 12, pause 400 ms)
...
== stage 2 done: 9 alive in 113.6 s

== results
subscriptions       : 2
configs stored      : 4314
checks executed     : 462 in 19 batches
first working node  : 1.93 s
stage 1 ( 5 nodes) :   7.86 s -> 5 alive
stage 2 (12 nodes): 113.61 s -> 9 alive
stage 3 (trickle)   :   0.00 s -> 9 alive
alive vs dead (db)  : 9 / 453 (1.9%)
new nodes per check : 1.9% (9 of 462 checks)
protocols           : ss=8 ssr=1
delay p50/p90       : 507 / 1410 ms
best node           : GB-node (ss) 445 ms
verify through      : GB-node -> HTTP 204 in 670 ms
```

## Options

```text
--db PATH             sqlite database (default data/discovery.db)
--fast N              stage 1 target (default 8)
--bulk N              stage 2 target (default 100)
--batch N             stage 1 batch size (default 64)
--concurrency N       stage 1 parallel checks (default 64)
--bulk-batch N        stage 2 batch size (default 16)
--bulk-concurrency N  stage 2 parallel checks (default 8)
--bulk-pause MS       pause between stage 2 batches (default 250)
--trickle-every S     stage 3 interval (default 20)
--trickle-batch N     stage 3 batch size (default 4)
--refresh S           subscription refresh interval (default 3600)
--run-seconds N       stop after N seconds (0 = until Ctrl+C)
--no-verify           skip the probe through the best node
--json PATH           write a machine-readable summary
```

## Reading the numbers

- **first working node** is the latency from start to the first alive config; a
  value under a couple of seconds means the fast stage is doing its job.
- **stage 1** should stay short: it is the "give me something usable now" path.
- **stage 2** grows the pool and is intentionally slower per check; if it takes
  minutes, the subscription simply has few working nodes, which the success rate
  shows.
- **alive vs dead** is the dataset's alive share after the run; **new nodes per
  check** is how many working nodes each check produced in this run. Public free
  subscriptions usually land between 2% and 5%.
- **verify** selects the best rated node, routes the process through it and
  requests `generate_204`, so the run ends with proof that discovery produced
  traffic, not just database rows.

The database is reused between runs: configs already marked alive are not
re-tested by stages 1 and 2, dead ones are. Delete the file to measure a cold
start.

Request pacing is disabled inside the library (`request_limit = 0`); the stages
pace themselves through batch size, concurrency and pauses.
