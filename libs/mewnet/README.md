# mewnet Go API

Importable Go API for the mihomo-backed core. It wraps `internal/core` and
does not require cgo.

```go
import "mewnet"

client, err := mewnet.Start(mewnet.Config{
    MixedPort: -1,       // -1 disable, 0 free port/YAML, >0 exact
    LogLevel:  "warning",
})
if err != nil {
    return err
}
defer client.Close()

selectProxy := true
added, err := client.Add(vlessLink, mewnet.AddOptions{Select: &selectProxy})
if err != nil {
    return err
}
_ = added

delay, err := client.TestDelay(added.Added[0].Name, mewnet.DelayOptions{
    URL:       "https://www.gstatic.com/generate_204",
    Expected:  "204",
    TimeoutMS: 5000,
})
```

## Batch checks

```go
results := client.TestLinksBatch(ctx, links, mewnet.DelayOptions{
    URL:       "https://www.gstatic.com/generate_204",
    Expected:  "204",
    TimeoutMS: 5000,
}, 70)

first := client.TestLinksBatchFirst(ctx, links, opts, 70)
```

`TestLinksBatchFirst` cancels other checks when the first link succeeds.

## Fingerprint

```go
id := mewnet.Fingerprint(link) // lowercase hex SHA-256
raw := mewnet.FingerprintBytes(link)
```

The identity is `protocol + host + port + credentials`; names and transport
options are ignored. The same function is used by subscription deduplication.

## SharedVPNLink

```go
link, err := client.CreateLink(vlessLink, mewnet.LinkOptions{
    LocalProxy: boolPtr(true),
})
defer client.DestroyLink(link.ID)

info, _ := client.LinkInfo(link.ID)
ping, _ := client.LinkPing(link.ID, mewnet.DelayOptions{})
speed, _ := client.LinkSpeed(link.ID, mewnet.SpeedOptions{})
exitIP, _ := client.LinkExitIP(link.ID, mewnet.ExitIPOptions{})
```

Async variants return a task id:

```go
id := client.LinkPingAsync(link.ID, opts, func(snapshot mewnet.TaskSnapshot) {
    // callback runs on an arbitrary goroutine
})
```

## Probe through the local mixed proxy

```go
status, delay, err := client.ProbeHTTP("https://lua.org", 200, 5000)
```

## Scope

This Go package exposes the **core** (Mihomo, proxies, links, tests, probe,
fingerprint). Subscriptions, SQLite storage and the discovery agent are the C
part of the project and are currently used from C/CLI/GTK. They are not exposed
by this pure-Go package yet.
