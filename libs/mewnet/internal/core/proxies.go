package core

import (
	"errors"
	"fmt"
	"net"
	"net/url"
	"strconv"
	"strings"
	"time"

	"github.com/metacubex/mihomo/adapter"
	"github.com/metacubex/mihomo/common/convert"
	C "github.com/metacubex/mihomo/constant"
	"github.com/metacubex/mihomo/tunnel"
	"github.com/metacubex/mihomo/tunnel/statistic"
)

var reservedNames = map[string]bool{
	"DIRECT":      true,
	"REJECT":      true,
	"REJECT-DROP": true,
	"COMPATIBLE":  true,
	"PASS":        true,
	"PASS-RULE":   true,
	"GLOBAL":      true,
	GroupName:     true,
}

// AddOptions configures mewnet_add_proxy.
type AddOptions struct {
	Name           string
	Select         *bool
	SkipCertVerify bool
}

// ProxyInfo is the public description of a proxy group member.
type ProxyInfo struct {
	Name        string
	Type        string
	Server      string
	Port        int
	Source      string
	Alive       bool
	Delay       uint16
	LastTestURL string
}

// AddResult is returned by Add.
type AddResult struct {
	Added    []ProxyInfo
	Selected string
}

type stateSnapshot struct {
	entries  map[string]*entry
	order    []string
	excluded map[string]bool
	selected string
}

func (c *Core) snapshotState() stateSnapshot {
	entries := make(map[string]*entry, len(c.entries))
	for k, v := range c.entries {
		entries[k] = v
	}
	excluded := make(map[string]bool, len(c.excluded))
	for k, v := range c.excluded {
		excluded[k] = v
	}
	return stateSnapshot{
		entries:  entries,
		order:    append([]string(nil), c.order...),
		excluded: excluded,
		selected: c.selected,
	}
}

func (c *Core) restoreState(s stateSnapshot) {
	c.entries = s.entries
	c.order = s.order
	c.excluded = s.excluded
	c.selected = s.selected
}

// Add parses one or more share links and hot-swaps them into the MEWNET group.
func (c *Core) Add(linkOrBlob string, opts AddOptions) (*AddResult, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return nil, ErrClosed
	}

	mappings, err := convert.ConvertsV2Ray([]byte(linkOrBlob))
	if err != nil {
		return nil, fmt.Errorf("parse proxy links: %w", err)
	}
	if len(mappings) == 0 {
		return nil, errors.New("no supported proxy links found")
	}

	taken := c.namesLocked()
	prepared := make([]*entry, 0, len(mappings))

	for i, mapping := range mappings {
		typ, _ := mapping["type"].(string)
		server, _ := mapping["server"].(string)
		port, portErr := toInt(mapping["port"])
		if typ == "" || server == "" || portErr != nil || port <= 0 || port > 65535 {
			return nil, fmt.Errorf("proxy %d: missing or invalid type/server/port", i)
		}

		if opts.SkipCertVerify {
			mapping["skip-cert-verify"] = true
		}
		if len(mappings) == 1 {
			applyInsecureFromLink(linkOrBlob, mapping)
		}

		name := ""
		if len(mappings) == 1 && opts.Name != "" {
			name = opts.Name
		} else {
			name, _ = mapping["name"].(string)
		}
		name = sanitizeName(name)
		if name == "" {
			name = fmt.Sprintf("%s-%s-%d", typ, server, port)
		}
		name = uniqueName(name, taken)
		taken[name] = true
		mapping["name"] = name

		tmp, err := adapter.ParseProxy(mapping)
		if err != nil {
			return nil, fmt.Errorf("proxy %q: %w", name, err)
		}
		_ = tmp.Close()

		prepared = append(prepared, &entry{
			Name:    name,
			Type:    strings.ToUpper(typ),
			Server:  server,
			Port:    port,
			Mapping: mapping,
			Source:  "link",
			AddedAt: time.Now(),
		})
	}

	before := c.snapshotState()
	for _, e := range prepared {
		c.entries[e.Name] = e
		c.order = append(c.order, e.Name)
	}
	if opts.Select == nil || *opts.Select {
		c.selected = prepared[0].Name
	}

	if err := c.rebuildLocked(); err != nil {
		c.restoreState(before)
		_ = c.rebuildLocked()
		return nil, fmt.Errorf("apply proxy change: %w", err)
	}

	added := make([]ProxyInfo, 0, len(prepared))
	for _, e := range prepared {
		added = append(added, c.infoForNameLocked(e.Name, e.Source))
	}
	return &AddResult{Added: added, Selected: c.selected}, nil
}

// Remove removes a proxy from the runtime MEWNET group.
func (c *Core) Remove(name string) (*ProxyInfo, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return nil, ErrClosed
	}
	if name == "" {
		return nil, errors.New("name is required")
	}
	if reservedNames[name] {
		return nil, fmt.Errorf("proxy %q is built-in and cannot be removed", name)
	}
	if link, ok := c.linkByName(name); ok {
		return nil, fmt.Errorf("proxy %q belongs to link %d; use mewnet_link_destroy", name, link.ID)
	}

	before := c.snapshotState()
	info := c.infoForNameLocked(name, "")
	if info.Type == "" {
		return nil, fmt.Errorf("proxy %q not found", name)
	}

	removed := false
	if e, ok := c.entries[name]; ok {
		delete(c.entries, name)
		c.order = removeString(c.order, name)
		removed = true
		_ = e
	} else if _, ok := c.baseMeta[name]; ok {
		c.excluded[name] = true
		removed = true
	}
	if !removed {
		return nil, fmt.Errorf("proxy %q not found", name)
	}

	if c.selected == name {
		c.selected = "DIRECT"
	}

	if err := c.rebuildLocked(); err != nil {
		c.restoreState(before)
		_ = c.rebuildLocked()
		return nil, fmt.Errorf("apply proxy change: %w", err)
	}

	closeConnectionsWithChain(name)
	return &info, nil
}

// Select changes the selected proxy of a selector group.
func (c *Core) Select(group, name string) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return ErrClosed
	}
	if group == "" {
		group = GroupName
	}
	proxy, ok := tunnel.Proxies()[group]
	if !ok {
		return fmt.Errorf("proxy group %q not found", group)
	}
	selector, ok := proxy.Adapter().(interface{ Set(string) error })
	if !ok {
		return fmt.Errorf("%q is not a selector group", group)
	}
	if err := selector.Set(name); err != nil {
		return fmt.Errorf("select %q in %q: %w", name, group, err)
	}
	if group == GroupName {
		c.selected = name
	}
	return nil
}

// ProxyList is the current membership of the MEWNET group.
type ProxyList struct {
	Selected string
	Proxies  []ProxyInfo
}

// List returns the current MEWNET group members.
func (c *Core) List() (*ProxyList, error) {
	c.mu.RLock()
	if c.closed {
		c.mu.RUnlock()
		return nil, ErrClosed
	}
	members := c.groupMembersLocked()
	selected := c.selected
	c.mu.RUnlock()

	infos := make([]ProxyInfo, 0, len(members))
	for _, proxy := range members {
		infos = append(infos, c.infoFromProxy(proxy))
	}
	if selected == "" {
		selected = currentSelection()
	}
	return &ProxyList{Selected: selected, Proxies: infos}, nil
}

func (c *Core) groupMembersLocked() []C.Proxy {
	proxy, ok := tunnel.Proxies()[GroupName]
	if !ok {
		return nil
	}
	if group, ok := proxy.Adapter().(interface{ Proxies() []C.Proxy }); ok {
		return group.Proxies()
	}
	return nil
}

func (c *Core) infoForNameLocked(name, source string) ProxyInfo {
	proxy, ok := tunnel.Proxies()[name]
	if !ok {
		return ProxyInfo{}
	}
	info := c.infoFromProxy(proxy)
	if source != "" {
		info.Source = source
	}
	return info
}

func (c *Core) infoFromProxy(proxy C.Proxy) ProxyInfo {
	name := proxy.Name()
	info := ProxyInfo{
		Name:   name,
		Type:   strings.ToUpper(proxy.Type().String()),
		Source: "builtin",
	}

	if e, ok := c.entries[name]; ok {
		info.Server = e.Server
		info.Port = e.Port
		info.Source = e.Source
		info.Type = e.Type
	} else if meta, ok := c.baseMeta[name]; ok {
		info.Server = meta.Server
		info.Port = meta.Port
		info.Source = "config"
		info.Type = meta.Type
	} else if addr := proxy.Addr(); addr != "" {
		info.Server, info.Port = splitHostPort(addr)
	}

	if history := proxy.DelayHistory(); len(history) > 0 {
		last := history[len(history)-1]
		info.Delay = last.Delay
		info.Alive = last.Delay > 0
		info.LastTestURL = C.DefaultTestURL
	} else {
		info.Alive = proxy.AliveForTestUrl(C.DefaultTestURL)
	}
	return info
}

func (c *Core) namesLocked() map[string]bool {
	names := make(map[string]bool, len(reservedNames)+len(c.baseMeta)+len(c.entries))
	for name := range reservedNames {
		names[name] = true
	}
	for name := range c.baseMeta {
		names[name] = true
	}
	for name := range c.entries {
		names[name] = true
	}
	for _, group := range c.baseGroups {
		if name, _ := group["name"].(string); name != "" {
			names[name] = true
		}
	}
	return names
}

// rebuildLocked rebuilds the whole proxies map from the base config plus the
// managed entries and swaps it into the tunnel. c.mu must be held.
func (c *Core) rebuildLocked() error {
	membership := make([]string, 0, len(c.order)+1)
	seen := make(map[string]bool, len(c.order)+1)
	addMember := func(name string) {
		if name == "" || c.excluded[name] || seen[name] {
			return
		}
		seen[name] = true
		membership = append(membership, name)
	}

	for _, group := range c.baseGroups {
		if name, _ := group["name"].(string); name == GroupName {
			baseMembers, _ := listOfStrings(group["proxies"])
			for _, member := range baseMembers {
				addMember(member)
			}
		}
	}
	for _, name := range c.order {
		addMember(name)
	}
	if len(membership) == 0 {
		membership = []string{"DIRECT"}
	}

	selected := c.selected
	if !containsString(membership, selected) {
		selected = membership[0]
	}

	groups := make([]map[string]any, 0, len(c.baseGroups)+1)
	foundGroup := false
	for _, group := range c.baseGroups {
		cp := deepCopyAny(group).(map[string]any)
		if name, _ := cp["name"].(string); name == GroupName {
			foundGroup = true
			cp["type"] = "select"
			cp["proxies"] = stringsToList(membership)
			cp["default-selected"] = selected
		}
		groups = append(groups, cp)
	}
	if !foundGroup {
		groups = append(groups, map[string]any{
			"name":             GroupName,
			"type":             "select",
			"proxies":          stringsToList(membership),
			"default-selected": selected,
		})
	}

	proxiesRaw := make([]map[string]any, 0, len(c.baseProxies)+len(c.order))
	for _, mapping := range c.baseProxies {
		name, _ := mapping["name"].(string)
		if c.excluded[name] {
			continue
		}
		proxiesRaw = append(proxiesRaw, deepCopyAny(mapping).(map[string]any))
	}
	for _, name := range c.order {
		if c.excluded[name] {
			continue
		}
		if e, ok := c.entries[name]; ok && e != nil {
			proxiesRaw = append(proxiesRaw, deepCopyAny(e.Mapping).(map[string]any))
		}
	}

	newProxies, newProviders, err := buildProxies(proxiesRaw, groups)
	if err != nil {
		return err
	}

	oldProxies := tunnel.Proxies()
	oldProviders := tunnel.Providers()
	tunnel.UpdateProxies(newProxies, newProviders)
	closeReplaced(oldProxies, oldProviders, newProxies, newProviders)

	if group, ok := newProxies[GroupName]; ok {
		if now, ok := group.Adapter().(interface{ Now() string }); ok {
			if name := now.Now(); name != "" {
				c.selected = name
			}
		}
	}
	return nil
}

func closeConnectionsWithChain(name string) {
	statistic.DefaultManager.Range(func(t statistic.Tracker) bool {
		for _, chain := range t.Info().Chain {
			if chain == name {
				_ = t.Close()
				return true
			}
		}
		return true
	})
}

func sanitizeName(name string) string {
	name = strings.TrimSpace(name)
	name = strings.Map(func(r rune) rune {
		switch r {
		case '\n', '\r', '\t':
			return ' '
		}
		return r
	}, name)
	name = strings.TrimSpace(name)
	runes := []rune(name)
	if len(runes) > 100 {
		name = string(runes[:100])
	}
	return name
}

func uniqueName(name string, taken map[string]bool) string {
	if !taken[name] {
		return name
	}
	for i := 2; ; i++ {
		candidate := fmt.Sprintf("%s-%d", name, i)
		if !taken[candidate] {
			return candidate
		}
	}
}

func applyInsecureFromLink(link string, mapping map[string]any) {
	line := strings.TrimSpace(link)
	if line == "" || strings.ContainsAny(line, "\r\n") {
		return
	}
	u, err := url.Parse(line)
	if err != nil {
		return
	}
	query := u.Query()
	raw := query.Get("allowInsecure")
	if raw == "" {
		raw = query.Get("insecure")
	}
	if raw == "" {
		return
	}
	if insecure, err := strconv.ParseBool(raw); err == nil && insecure {
		mapping["skip-cert-verify"] = true
	}
}

func removeString(list []string, target string) []string {
	out := list[:0]
	for _, item := range list {
		if item != target {
			out = append(out, item)
		}
	}
	return out
}

func containsString(list []string, target string) bool {
	for _, item := range list {
		if item == target {
			return true
		}
	}
	return false
}

func splitHostPort(addr string) (string, int) {
	host, portStr, err := net.SplitHostPort(addr)
	if err != nil {
		return addr, 0
	}
	port, _ := strconv.Atoi(portStr)
	return host, port
}
