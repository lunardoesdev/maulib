package core

import (
	"fmt"
	"sort"

	"github.com/metacubex/mihomo/adapter"
	"github.com/metacubex/mihomo/adapter/outbound"
	"github.com/metacubex/mihomo/adapter/outboundgroup"
	"github.com/metacubex/mihomo/adapter/provider"
	C "github.com/metacubex/mihomo/constant"
	P "github.com/metacubex/mihomo/constant/provider"
	"github.com/metacubex/mihomo/tunnel"
)

// buildProxies mirrors the relevant part of config.parseProxies: it creates
// the built-in proxies, parses user proxies and proxy groups, builds the
// reserved "default" provider and the automatic GLOBAL group.
func buildProxies(proxyMappings []map[string]any, groupMappings []map[string]any) (map[string]C.Proxy, map[string]P.ProxyProvider, error) {
	proxies := make(map[string]C.Proxy)
	providersMap := make(map[string]P.ProxyProvider)

	proxies["DIRECT"] = adapter.NewProxy(outbound.NewDirect())
	proxies["REJECT"] = adapter.NewProxy(outbound.NewReject())
	proxies["REJECT-DROP"] = adapter.NewProxy(outbound.NewRejectDrop())
	proxies["COMPATIBLE"] = adapter.NewProxy(outbound.NewCompatible())
	proxies["PASS"] = adapter.NewProxy(outbound.NewPass())
	proxies["PASS-RULE"] = adapter.NewProxy(outbound.NewPassRule())

	proxyList := []string{"DIRECT", "REJECT"}
	allProxies := make([]string, 0, len(proxyMappings))

	for idx, mapping := range proxyMappings {
		name, _ := mapping["name"].(string)
		if name == "" {
			return nil, nil, fmt.Errorf("proxy %d: missing name", idx)
		}
		if _, exists := proxies[name]; exists {
			return nil, nil, fmt.Errorf("proxy %s is the duplicate name", name)
		}
		proxy, err := adapter.ParseProxy(mapping, adapter.WithTunnelForAPI(tunnel.Tunnel))
		if err != nil {
			return nil, nil, fmt.Errorf("proxy %s: %w", name, err)
		}
		proxies[name] = proxy
		proxyList = append(proxyList, name)
		allProxies = append(allProxies, name)
	}

	sortedProxies := append([]string(nil), allProxies...)
	sort.Strings(sortedProxies)
	allProviders := []string{}

	// Groups may reference other groups; parse them in dependency order.
	remaining := append([]map[string]any(nil), groupMappings...)
	for len(remaining) > 0 {
		progressed := false
		var next []map[string]any
		var lastErr error

		for _, mapping := range remaining {
			group, err := outboundgroup.ParseProxyGroup(mapping, proxies, providersMap, sortedProxies, allProviders)
			if err != nil {
				lastErr = err
				next = append(next, mapping)
				continue
			}
			name := group.Name()
			if _, exists := proxies[name]; exists {
				lastErr = fmt.Errorf("proxy group %s: the duplicate name", name)
				next = append(next, mapping)
				continue
			}
			proxies[name] = adapter.NewProxy(group)
			proxyList = append(proxyList, name)
			progressed = true
		}

		if !progressed {
			if lastErr == nil {
				lastErr = fmt.Errorf("could not parse proxy groups")
			}
			return nil, nil, lastErr
		}
		remaining = next
	}

	ps := make([]C.Proxy, 0, len(proxyList))
	for _, name := range proxyList {
		proxy, ok := proxies[name]
		if !ok {
			continue
		}
		if proxy.Type() == C.Pass || proxy.Type() == C.PassRule {
			continue
		}
		ps = append(ps, proxy)
	}
	hc := provider.NewHealthCheck(ps, "", 5000, 0, true, nil)
	pd, err := provider.NewCompatibleProvider(provider.ReservedName, ps, hc)
	if err != nil {
		return nil, nil, fmt.Errorf("create default provider: %w", err)
	}
	providersMap[provider.ReservedName] = pd

	hasGlobal := false
	for _, mapping := range groupMappings {
		if name, _ := mapping["name"].(string); name == "GLOBAL" {
			hasGlobal = true
			break
		}
	}
	if !hasGlobal {
		global, err := outboundgroup.NewSelector(
			outboundgroup.GroupCommonOption{Name: "GLOBAL"},
			outboundgroup.SelectorOption{},
			proxies["COMPATIBLE"],
			[]P.ProxyProvider{pd},
		)
		if err != nil {
			return nil, nil, fmt.Errorf("create GLOBAL group: %w", err)
		}
		proxies["GLOBAL"] = adapter.NewProxy(global)
	}

	return proxies, providersMap, nil
}

func closeReplaced(oldProxies map[string]C.Proxy, oldProviders map[string]P.ProxyProvider, newProxies map[string]C.Proxy, newProviders map[string]P.ProxyProvider) {
	for name, old := range oldProxies {
		cur, ok := newProxies[name]
		if ok && cur == old {
			continue
		}
		_ = old.Close()
	}
	for name, old := range oldProviders {
		cur, ok := newProviders[name]
		if ok && cur == old {
			continue
		}
		closeProvider(old)
	}
}

// closeProvider closes a provider when its concrete type supports it. The
// ProxyProvider interface itself has no Close method.
func closeProvider(p P.ProxyProvider) {
	if closer, ok := p.(interface{ Close() error }); ok {
		_ = closer.Close()
	}
}

// deepCopyAny copies the YAML-like structures used by the raw config.
func deepCopyAny(v any) any {
	switch t := v.(type) {
	case map[string]any:
		out := make(map[string]any, len(t))
		for k, item := range t {
			out[k] = deepCopyAny(item)
		}
		return out
	case []any:
		out := make([]any, len(t))
		for i, item := range t {
			out[i] = deepCopyAny(item)
		}
		return out
	case []map[string]any:
		out := make([]map[string]any, len(t))
		for i, item := range t {
			out[i] = deepCopyAny(item).(map[string]any)
		}
		return out
	default:
		return v
	}
}
