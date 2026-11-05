package core

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/netip"
	"strings"
	"sync"
	"time"

	"github.com/metacubex/mihomo/adapter"
	"github.com/metacubex/mihomo/adapter/inbound"
	"github.com/metacubex/mihomo/common/convert"
	"github.com/metacubex/mihomo/component/auth"
	C "github.com/metacubex/mihomo/constant"
	LC "github.com/metacubex/mihomo/listener/config"
	"github.com/metacubex/mihomo/listener/socks"
	"github.com/metacubex/mihomo/tunnel"
)

const (
	defaultSpeedURL   = "https://speed.cloudflare.com/__down?bytes=%d"
	defaultSpeedBytes = int64(5 << 20)
	maxSpeedBytes     = int64(64 << 20)
	defaultExitIPURL  = "https://api.ipify.org"
	maxExitIPBody     = 64 << 10
)

// LinkOptions maps the JSON options accepted by milib_link_create.
//
// Username/Password protect the per-link local SOCKS5 endpoint. When they are
// empty a username and a random password are generated.
//
// ProxyPassword optionally overrides the upstream credential taken from the
// share link (uuid for VLESS/VMess, password for Trojan/Shadowsocks/...).
type LinkOptions struct {
	Name          string `json:"name"`
	Select        *bool  `json:"select"`
	LocalProxy    *bool  `json:"local_proxy"`
	Username      string `json:"username"`
	Password      string `json:"password"`
	ProxyPassword string `json:"proxy_password"`
}

// SpeedOptions maps the JSON options accepted by speed tests.
type SpeedOptions struct {
	URL       string `json:"url"`
	Bytes     int64  `json:"bytes"`
	TimeoutMS int    `json:"timeout_ms"`
}

// ExitIPOptions maps the JSON options accepted by exit-IP lookups.
type ExitIPOptions struct {
	URL       string `json:"url"`
	Format    string `json:"format"`
	TimeoutMS int    `json:"timeout_ms"`
}

// SharedLink is the server-side representation of a SharedVPNLink handle.
type SharedLink struct {
	ID       uint64
	Name     string
	Link     string
	Type     string
	Server   string
	Port     int
	Username string
	Password string
	Socks    string

	listener io.Closer
}

type staticAuthStore struct {
	mu sync.RWMutex
	a  auth.Authenticator
}

func (s *staticAuthStore) Authenticator() auth.Authenticator {
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.a
}

func (s *staticAuthStore) SetAuthenticator(a auth.Authenticator) {
	s.mu.Lock()
	s.a = a
	s.mu.Unlock()
}

// linkTunnel routes every accepted SOCKS5 connection through one proxy,
// bypassing the global selector.
type linkTunnel struct {
	core *Core
	name string
}

func (t *linkTunnel) HandleTCPConn(conn net.Conn, metadata *C.Metadata) {
	proxy, ok := tunnel.Proxies()[t.name]
	if !ok {
		_ = conn.Close()
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	remote, err := proxy.DialContext(ctx, metadata)
	if err != nil {
		_ = conn.Close()
		return
	}

	relay(conn, remote)
}

func (t *linkTunnel) HandleUDPPacket(packet C.UDPPacket, _ *C.Metadata) {
	packet.Drop()
}

func (t *linkTunnel) NatTable() C.NatTable {
	return nil
}

func relay(a, b net.Conn) {
	var wg sync.WaitGroup
	wg.Add(2)
	go func() {
		defer wg.Done()
		_, _ = io.Copy(a, b)
		closeWrite(a)
	}()
	go func() {
		defer wg.Done()
		_, _ = io.Copy(b, a)
		closeWrite(b)
	}()
	wg.Wait()
	_ = a.Close()
	_ = b.Close()
}

func closeWrite(conn net.Conn) {
	if cw, ok := conn.(interface{ CloseWrite() error }); ok {
		_ = cw.CloseWrite()
	}
}

// CreateLink parses a share link, creates a proxy, adds it to the MILIB group
// and starts a per-link authenticated SOCKS5 endpoint.
func (c *Core) CreateLink(link string, opts LinkOptions) (*SharedLink, error) {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return nil, ErrClosed
	}
	if strings.TrimSpace(link) == "" {
		return nil, errors.New("link is required")
	}

	mappings, err := convert.ConvertsV2Ray([]byte(link))
	if err != nil {
		return nil, fmt.Errorf("parse proxy link: %w", err)
	}
	if len(mappings) != 1 {
		return nil, fmt.Errorf("expected exactly one proxy link, got %d", len(mappings))
	}
	mapping := mappings[0]

	typ, _ := mapping["type"].(string)
	server, _ := mapping["server"].(string)
	port, portErr := toInt(mapping["port"])
	if typ == "" || server == "" || portErr != nil || port <= 0 || port > 65535 {
		return nil, errors.New("proxy link: missing or invalid type/server/port")
	}
	applyInsecureFromLink(link, mapping)
	if opts.ProxyPassword != "" {
		applyProxyPassword(mapping, opts.ProxyPassword)
	}

	taken := c.namesLocked()
	name := opts.Name
	if name == "" {
		name, _ = mapping["name"].(string)
	}
	name = sanitizeName(name)
	if name == "" {
		name = fmt.Sprintf("%s-%s-%d", typ, server, port)
	}
	name = uniqueName(name, taken)
	mapping["name"] = name

	tmp, err := adapter.ParseProxy(mapping)
	if err != nil {
		return nil, fmt.Errorf("proxy %q: %w", name, err)
	}
	_ = tmp.Close()

	username := opts.Username
	if username == "" {
		username = "milib"
	}
	password := opts.Password
	if password == "" {
		password, err = randomKey(18)
		if err != nil {
			return nil, fmt.Errorf("generate access key: %w", err)
		}
	}

	before := c.snapshotState()
	c.entries[name] = &entry{
		Name:    name,
		Type:    strings.ToUpper(typ),
		Server:  server,
		Port:    port,
		Mapping: mapping,
		Source:  "link",
		AddedAt: time.Now(),
	}
	c.order = append(c.order, name)
	if opts.Select != nil && *opts.Select {
		c.selected = name
	}
	if err := c.rebuildLocked(); err != nil {
		c.restoreState(before)
		_ = c.rebuildLocked()
		return nil, fmt.Errorf("apply proxy: %w", err)
	}

	shared := &SharedLink{
		Name:     name,
		Link:     link,
		Type:     strings.ToUpper(typ),
		Server:   server,
		Port:     port,
		Username: username,
		Password: password,
	}

	localProxy := opts.LocalProxy == nil || *opts.LocalProxy
	if localProxy {
		addr, listener, err := startLinkSocks(c, name, username, password)
		if err != nil {
			delete(c.entries, name)
			c.order = removeString(c.order, name)
			_ = c.rebuildLocked()
			return nil, fmt.Errorf("start local proxy: %w", err)
		}
		shared.Socks = addr
		shared.listener = listener
	}

	c.nextLinkID++
	shared.ID = c.nextLinkID
	c.links[shared.ID] = shared
	return shared, nil
}

// DestroyLink removes a link, its proxy and its local SOCKS endpoint.
func (c *Core) DestroyLink(id uint64) error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return ErrClosed
	}
	link, ok := c.links[id]
	if !ok {
		return fmt.Errorf("link %d not found", id)
	}
	delete(c.links, id)
	if link.listener != nil {
		_ = link.listener.Close()
	}

	if _, ok := c.entries[link.Name]; ok {
		delete(c.entries, link.Name)
		c.order = removeString(c.order, link.Name)
	}
	if c.selected == link.Name {
		c.selected = "DIRECT"
	}
	if err := c.rebuildLocked(); err != nil {
		return fmt.Errorf("apply proxy removal: %w", err)
	}
	closeConnectionsWithChain(link.Name)
	return nil
}

// LinkInfo returns the public description of a link.
func (c *Core) LinkInfo(id uint64) (map[string]any, error) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	if c.closed {
		return nil, ErrClosed
	}
	link, ok := c.links[id]
	if !ok {
		return nil, fmt.Errorf("link %d not found", id)
	}
	return map[string]any{
		"link_id":  link.ID,
		"name":     link.Name,
		"type":     link.Type,
		"server":   link.Server,
		"port":     link.Port,
		"link":     link.Link,
		"selected": c.selected == link.Name,
		"socks": map[string]any{
			"addr":     link.Socks,
			"username": link.Username,
			"password": link.Password,
		},
		"auth": map[string]any{
			"username": link.Username,
			"password": link.Password,
		},
	}, nil
}

// ListLinks returns all live links.
func (c *Core) ListLinks() ([]map[string]any, error) {
	c.mu.RLock()
	if c.closed {
		c.mu.RUnlock()
		return nil, ErrClosed
	}
	ids := make([]uint64, 0, len(c.links))
	for id := range c.links {
		ids = append(ids, id)
	}
	c.mu.RUnlock()

	sortUint64(ids)
	out := make([]map[string]any, 0, len(ids))
	for _, id := range ids {
		info, err := c.LinkInfo(id)
		if err == nil {
			out = append(out, info)
		}
	}
	return out, nil
}

func (c *Core) linkProxy(id uint64) (*SharedLink, C.Proxy, error) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	if c.closed {
		return nil, nil, ErrClosed
	}
	link, ok := c.links[id]
	if !ok {
		return nil, nil, fmt.Errorf("link %d not found", id)
	}
	proxy, ok := tunnel.Proxies()[link.Name]
	if !ok {
		return nil, nil, fmt.Errorf("proxy %q is not active", link.Name)
	}
	return link, proxy, nil
}

func (c *Core) linkByName(name string) (*SharedLink, bool) {
	for _, link := range c.links {
		if link.Name == name {
			return link, true
		}
	}
	return nil, false
}

// LinkPing measures HTTP latency by sending traffic through the link's proxy.
func (c *Core) LinkPing(id uint64, opts DelayOptions) (map[string]any, error) {
	return c.linkPingContext(context.Background(), id, opts)
}

func (c *Core) linkPingContext(ctx context.Context, id uint64, opts DelayOptions) (map[string]any, error) {
	resolved, err := opts.resolve()
	if err != nil {
		return nil, err
	}
	link, proxy, err := c.linkProxy(id)
	if err != nil {
		return nil, err
	}

	testCtx, cancel := context.WithTimeout(ctx, resolved.timeout)
	defer cancel()

	delay, err := safeURLTest(testCtx, proxy, resolved.url, resolved.expected)
	if err != nil {
		return nil, err
	}
	return map[string]any{
		"link_id": id,
		"name":    link.Name,
		"delay":   delay,
		"url":     resolved.url,
		"mode":    "proxy",
	}, nil
}

// LinkPingMS is the numeric form of LinkPing: it returns the delay in
// milliseconds and nothing else.
func (c *Core) LinkPingMS(id uint64, opts DelayOptions) (int, error) {
	result, err := c.LinkPing(id, opts)
	if err != nil {
		return 0, err
	}
	switch value := result["delay"].(type) {
	case uint16:
		return int(value), nil
	case int:
		return value, nil
	case int64:
		return int(value), nil
	case float64:
		return int(value), nil
	default:
		return 0, errors.New("delay value is missing")
	}
}

// LinkSpeed downloads data through the link's proxy and reports throughput.
func (c *Core) LinkSpeed(id uint64, opts SpeedOptions) (map[string]any, error) {
	return c.linkSpeedContext(context.Background(), id, opts)
}

func (c *Core) linkSpeedContext(ctx context.Context, id uint64, opts SpeedOptions) (map[string]any, error) {
	limit := opts.Bytes
	if limit <= 0 {
		limit = defaultSpeedBytes
	}
	if limit > maxSpeedBytes {
		limit = maxSpeedBytes
	}
	timeout := durationFromMS(opts.TimeoutMS, 15*time.Second, 120*time.Second)
	url := opts.URL
	if url == "" {
		url = fmt.Sprintf(defaultSpeedURL, limit)
	}

	link, proxy, err := c.linkProxy(id)
	if err != nil {
		return nil, err
	}

	reqCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	start := time.Now()
	req, err := http.NewRequestWithContext(reqCtx, http.MethodGet, url, nil)
	if err != nil {
		return nil, fmt.Errorf("build request: %w", err)
	}
	client := newProxyHTTPClient(proxy)
	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("speed request: %w", err)
	}
	defer resp.Body.Close()

	written, copyErr := io.Copy(io.Discard, io.LimitReader(resp.Body, limit))
	elapsed := time.Since(start)
	if copyErr != nil && written == 0 {
		return nil, fmt.Errorf("read body: %w", copyErr)
	}
	if written == 0 {
		return nil, errors.New("speed request returned no data")
	}
	seconds := elapsed.Seconds()
	bytesPerSecond := float64(written) / seconds
	return map[string]any{
		"link_id":          id,
		"name":             link.Name,
		"url":              url,
		"status":           resp.StatusCode,
		"bytes":            written,
		"seconds":          seconds,
		"bytes_per_second": bytesPerSecond,
		"mbps":             bytesPerSecond * 8 / 1_000_000,
	}, nil
}

// LinkExitIP resolves the exit IP by requesting an echo service through the
// link's proxy.
func (c *Core) LinkExitIP(id uint64, opts ExitIPOptions) (map[string]any, error) {
	return c.linkExitIPContext(context.Background(), id, opts)
}

func (c *Core) linkExitIPContext(ctx context.Context, id uint64, opts ExitIPOptions) (map[string]any, error) {
	timeout := durationFromMS(opts.TimeoutMS, 10*time.Second, 60*time.Second)
	url := opts.URL
	if url == "" {
		url = defaultExitIPURL
	}

	link, proxy, err := c.linkProxy(id)
	if err != nil {
		return nil, err
	}

	reqCtx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()

	req, err := http.NewRequestWithContext(reqCtx, http.MethodGet, url, nil)
	if err != nil {
		return nil, fmt.Errorf("build request: %w", err)
	}
	client := newProxyHTTPClient(proxy)
	resp, err := client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("exit ip request: %w", err)
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(io.LimitReader(resp.Body, maxExitIPBody))
	if err != nil {
		return nil, fmt.Errorf("read body: %w", err)
	}
	raw := strings.TrimSpace(string(body))
	ip := raw
	valid := false
	switch strings.ToLower(opts.Format) {
	case "", "text":
		if _, err := netip.ParseAddr(raw); err == nil {
			valid = true
		}
	case "json":
		var payload map[string]any
		if err := json.Unmarshal(body, &payload); err != nil {
			return nil, fmt.Errorf("parse json response: %w", err)
		}
		for _, key := range []string{"ip", "query", "origin"} {
			if value, ok := payload[key].(string); ok && value != "" {
				ip = strings.TrimSpace(strings.Split(value, ",")[0])
				break
			}
		}
		if _, err := netip.ParseAddr(ip); err == nil {
			valid = true
		}
	default:
		return nil, fmt.Errorf("unsupported format %q", opts.Format)
	}

	result := map[string]any{
		"link_id": id,
		"name":    link.Name,
		"url":     url,
		"status":  resp.StatusCode,
		"ip":      ip,
		"valid":   valid,
	}
	if !valid {
		result["raw"] = raw
	}
	return result, nil
}

// LinkPingAsync schedules a ping for one link.
func (c *Core) LinkPingAsync(id uint64, opts DelayOptions, cb func(int64, []byte)) int64 {
	return c.tasks.start(func(ctx context.Context) (any, error) {
		return c.linkPingContext(ctx, id, opts)
	}, cb)
}

// LinkSpeedAsync schedules a speed test for one link.
func (c *Core) LinkSpeedAsync(id uint64, opts SpeedOptions, cb func(int64, []byte)) int64 {
	return c.tasks.start(func(ctx context.Context) (any, error) {
		return c.linkSpeedContext(ctx, id, opts)
	}, cb)
}

// LinkExitIPAsync schedules an exit-IP lookup for one link.
func (c *Core) LinkExitIPAsync(id uint64, opts ExitIPOptions, cb func(int64, []byte)) int64 {
	return c.tasks.start(func(ctx context.Context) (any, error) {
		return c.linkExitIPContext(ctx, id, opts)
	}, cb)
}

// PingMany runs ping tests for all given links concurrently.
func (c *Core) PingMany(ids []uint64, opts DelayOptions) (map[string]any, error) {
	return c.pingManyContext(context.Background(), ids, opts)
}

func (c *Core) pingManyContext(ctx context.Context, ids []uint64, opts DelayOptions) (map[string]any, error) {
	if _, err := opts.resolve(); err != nil {
		return nil, err
	}
	results := make([]any, len(ids))
	var wg sync.WaitGroup
	for i, id := range ids {
		wg.Add(1)
		go func(index int, linkID uint64) {
			defer wg.Done()
			value, err := c.linkPingContext(ctx, linkID, opts)
			if err != nil {
				results[index] = map[string]any{
					"link_id": linkID,
					"ok":      false,
					"error":   err.Error(),
				}
				return
			}
			value["ok"] = true
			results[index] = value
		}(i, id)
	}
	wg.Wait()
	return map[string]any{"results": results, "count": len(ids)}, nil
}

// PingManyAsync schedules a concurrent ping of several links.
func (c *Core) PingManyAsync(ids []uint64, opts DelayOptions, cb func(int64, []byte)) int64 {
	return c.tasks.start(func(ctx context.Context) (any, error) {
		return c.pingManyContext(ctx, ids, opts)
	}, cb)
}

func startLinkSocks(c *Core, name, username, password string) (string, io.Closer, error) {
	store := &staticAuthStore{a: auth.NewAuthenticator([]auth.AuthUser{{User: username, Pass: password}})}
	config := LC.AuthServer{
		Enable:    true,
		Listen:    "127.0.0.1:0",
		AuthStore: store,
	}
	listener, err := socks.NewWithConfig(
		config,
		inbound.NewListenConfig(),
		&linkTunnel{core: c, name: name},
		inbound.WithInName("MILIB-LINK"),
		inbound.WithSpecialRules(""),
	)
	if err != nil {
		return "", nil, err
	}
	return listener.Address(), listener, nil
}

func newProxyHTTPClient(proxy C.Proxy) *http.Client {
	transport := &http.Transport{
		DisableKeepAlives: true,
		DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
			metadata := &C.Metadata{NetWork: C.TCP, Type: C.INNER}
			if err := metadata.SetRemoteAddress(addr); err != nil {
				return nil, err
			}
			return proxy.DialContext(ctx, metadata)
		},
	}
	return &http.Client{Transport: transport}
}

func applyProxyPassword(mapping map[string]any, password string) {
	typ, _ := mapping["type"].(string)
	switch strings.ToLower(typ) {
	case "vless", "vmess":
		mapping["uuid"] = password
	case "socks5", "http":
		mapping["username"] = "milib"
		mapping["password"] = password
	default:
		mapping["password"] = password
	}
}

func randomKey(bytesLen int) (string, error) {
	buf := make([]byte, bytesLen)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	return hex.EncodeToString(buf), nil
}

func durationFromMS(value int, fallback, max time.Duration) time.Duration {
	if value <= 0 {
		return fallback
	}
	d := time.Duration(value) * time.Millisecond
	if d > max {
		return max
	}
	return d
}

func sortUint64(values []uint64) {
	for i := 1; i < len(values); i++ {
		for j := i; j > 0 && values[j] < values[j-1]; j-- {
			values[j], values[j-1] = values[j-1], values[j]
		}
	}
}

// ParseLinkIDs decodes a JSON array of link ids.
func ParseLinkIDs(raw string) ([]uint64, error) {
	var ids []uint64
	if err := json.Unmarshal([]byte(raw), &ids); err != nil {
		return nil, fmt.Errorf("links must be a JSON array of link ids: %w", err)
	}
	if len(ids) == 0 {
		return nil, errors.New("links array is empty")
	}
	return ids, nil
}
