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

// LinkOptions configures a SharedVPNLink.
//
// Username/Password protect the per-link local SOCKS5 endpoint. When they are
// empty a username and a random password are generated.
//
// ProxyPassword optionally overrides the upstream credential taken from the
// share link (uuid for VLESS/VMess, password for Trojan/Shadowsocks/...).
type LinkOptions struct {
	Name          string
	Select        *bool
	LocalProxy    *bool
	Username      string
	Password      string
	ProxyPassword string
}

// SpeedOptions configures a throughput test.
type SpeedOptions struct {
	URL       string
	Bytes     int64
	TimeoutMS int
}

// ExitIPFormat selects how an exit-IP response is decoded.
type ExitIPFormat int

const (
	ExitIPFormatText ExitIPFormat = iota
	ExitIPFormatJSON
)

// ExitIPOptions configures an exit-IP lookup.
type ExitIPOptions struct {
	URL       string
	Format    ExitIPFormat
	TimeoutMS int
}

// LinkDescription is the public description of a SharedVPNLink.
type LinkDescription struct {
	ID       uint64
	Name     string
	Link     string
	Type     string
	Server   string
	Port     int
	Username string
	Password string
	Socks    string
	Selected bool
}

// PingResult is the outcome of a latency test through one link.
type PingResult struct {
	LinkID uint64
	Name   string
	URL    string
	Delay  uint16
}

// SpeedResult is the outcome of a throughput test through one link.
type SpeedResult struct {
	LinkID         uint64
	Name           string
	URL            string
	Status         int
	Bytes          int64
	Seconds        float64
	BytesPerSecond float64
	MBPS           float64
}

// ExitIPResult is the outcome of an exit-IP lookup through one link.
type ExitIPResult struct {
	LinkID uint64
	Name   string
	URL    string
	Status int
	IP     string
	Valid  bool
	Raw    string
}

// PingOutcome is one item of a concurrent multi-link ping.
type PingOutcome struct {
	LinkID uint64
	Name   string
	Delay  uint16
	Err    error
}

// PingManyResult is the outcome of a concurrent multi-link ping.
type PingManyResult struct {
	Outcomes []PingOutcome
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
	closing  bool

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

// CreateLink parses a share link, creates a proxy, adds it to the MEWNET group
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
		username = "mewnet"
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

// DestroyLink removes a link, its proxy and its local SOCKS endpoint. Async
// tasks that reference the link are cancelled and awaited first.
func (c *Core) DestroyLink(id uint64) error {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return ErrClosed
	}
	link, ok := c.links[id]
	if !ok {
		c.mu.Unlock()
		return fmt.Errorf("link %d not found", id)
	}
	if link.closing {
		c.mu.Unlock()
		return fmt.Errorf("link %d is being destroyed", id)
	}
	link.closing = true
	c.mu.Unlock()

	c.tasks.cancelLinks(id)

	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return ErrClosed
	}
	link, ok = c.links[id]
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
func (c *Core) LinkInfo(id uint64) (*LinkDescription, error) {
	c.mu.RLock()
	defer c.mu.RUnlock()
	if c.closed {
		return nil, ErrClosed
	}
	link, ok := c.links[id]
	if !ok {
		return nil, fmt.Errorf("link %d not found", id)
	}
	return &LinkDescription{
		ID:       link.ID,
		Name:     link.Name,
		Link:     link.Link,
		Type:     link.Type,
		Server:   link.Server,
		Port:     link.Port,
		Username: link.Username,
		Password: link.Password,
		Socks:    link.Socks,
		Selected: c.selected == link.Name,
	}, nil
}

// ListLinks returns all live links.
func (c *Core) ListLinks() ([]LinkDescription, error) {
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
	out := make([]LinkDescription, 0, len(ids))
	for _, id := range ids {
		info, err := c.LinkInfo(id)
		if err == nil {
			out = append(out, *info)
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
	if link.closing {
		return nil, nil, fmt.Errorf("link %d is being destroyed", id)
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

// SelectLink makes the link's proxy the selected proxy of the MEWNET group, so
// traffic entering the shared mixed inbound exits through that link.
func (c *Core) SelectLink(id uint64) error {
	c.mu.RLock()
	if c.closed {
		c.mu.RUnlock()
		return ErrClosed
	}
	link, ok := c.links[id]
	c.mu.RUnlock()
	if !ok {
		return fmt.Errorf("link %d not found", id)
	}
	return c.Select("", link.Name)
}

// LinkPing measures HTTP latency by sending traffic through the link's proxy.
func (c *Core) LinkPing(id uint64, opts DelayOptions) (*PingResult, error) {
	return c.linkPingContext(context.Background(), id, opts)
}

func (c *Core) linkPingContext(ctx context.Context, id uint64, opts DelayOptions) (*PingResult, error) {
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
	return &PingResult{
		LinkID: id,
		Name:   link.Name,
		Delay:  delay,
		URL:    resolved.url,
	}, nil
}

// LinkSpeed downloads data through the link's proxy and reports throughput.
func (c *Core) LinkSpeed(id uint64, opts SpeedOptions) (*SpeedResult, error) {
	return c.linkSpeedContext(context.Background(), id, opts)
}

func (c *Core) linkSpeedContext(ctx context.Context, id uint64, opts SpeedOptions) (*SpeedResult, error) {
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
	return &SpeedResult{
		LinkID:         id,
		Name:           link.Name,
		URL:            url,
		Status:         resp.StatusCode,
		Bytes:          written,
		Seconds:        seconds,
		BytesPerSecond: bytesPerSecond,
		MBPS:           bytesPerSecond * 8 / 1_000_000,
	}, nil
}

// LinkExitIP resolves the exit IP by requesting an echo service through the
// link's proxy.
func (c *Core) LinkExitIP(id uint64, opts ExitIPOptions) (*ExitIPResult, error) {
	return c.linkExitIPContext(context.Background(), id, opts)
}

func (c *Core) linkExitIPContext(ctx context.Context, id uint64, opts ExitIPOptions) (*ExitIPResult, error) {
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
	switch opts.Format {
	case ExitIPFormatText:
		if _, err := netip.ParseAddr(raw); err == nil {
			valid = true
		}
	case ExitIPFormatJSON:
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
		return nil, fmt.Errorf("unsupported exit ip format %d", opts.Format)
	}

	result := &ExitIPResult{
		LinkID: id,
		Name:   link.Name,
		URL:    url,
		Status: resp.StatusCode,
		IP:     ip,
		Valid:  valid,
	}
	if !valid {
		result.Raw = raw
	}
	return result, nil
}

// LinkPingAsync schedules a ping for one link.
func (c *Core) LinkPingAsync(id uint64, opts DelayOptions, cb func(TaskSnapshot)) int64 {
	return c.tasks.start(TaskKindLinkPing, []uint64{id}, func(ctx context.Context) (any, error) {
		return c.linkPingContext(ctx, id, opts)
	}, cb)
}

// LinkSpeedAsync schedules a speed test for one link.
func (c *Core) LinkSpeedAsync(id uint64, opts SpeedOptions, cb func(TaskSnapshot)) int64 {
	return c.tasks.start(TaskKindLinkSpeed, []uint64{id}, func(ctx context.Context) (any, error) {
		return c.linkSpeedContext(ctx, id, opts)
	}, cb)
}

// LinkExitIPAsync schedules an exit-IP lookup for one link.
func (c *Core) LinkExitIPAsync(id uint64, opts ExitIPOptions, cb func(TaskSnapshot)) int64 {
	return c.tasks.start(TaskKindLinkExitIP, []uint64{id}, func(ctx context.Context) (any, error) {
		return c.linkExitIPContext(ctx, id, opts)
	}, cb)
}

// PingMany runs ping tests for all given links concurrently.
func (c *Core) PingMany(ids []uint64, opts DelayOptions) (*PingManyResult, error) {
	return c.pingManyContext(context.Background(), ids, opts)
}

func (c *Core) pingManyContext(ctx context.Context, ids []uint64, opts DelayOptions) (*PingManyResult, error) {
	if _, err := opts.resolve(); err != nil {
		return nil, err
	}
	if len(ids) == 0 {
		return nil, errors.New("links array is empty")
	}
	outcomes := make([]PingOutcome, len(ids))
	var wg sync.WaitGroup
	for i, id := range ids {
		wg.Add(1)
		go func(index int, linkID uint64) {
			defer wg.Done()
			value, err := c.linkPingContext(ctx, linkID, opts)
			if err != nil {
				outcomes[index] = PingOutcome{LinkID: linkID, Err: err}
				return
			}
			outcomes[index] = PingOutcome{
				LinkID: linkID,
				Name:   value.Name,
				Delay:  value.Delay,
			}
		}(i, id)
	}
	wg.Wait()
	return &PingManyResult{Outcomes: outcomes}, nil
}

// PingManyAsync schedules a concurrent ping of several links.
func (c *Core) PingManyAsync(ids []uint64, opts DelayOptions, cb func(TaskSnapshot)) int64 {
	return c.tasks.start(TaskKindPingMany, ids, func(ctx context.Context) (any, error) {
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
		inbound.WithInName("MEWNET-LINK"),
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
		mapping["username"] = "mewnet"
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
