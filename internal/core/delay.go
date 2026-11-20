package core

import (
	"context"
	"fmt"
	"io"
	"math"
	"net"
	"net/url"
	"time"

	"github.com/metacubex/http"
	"github.com/metacubex/mihomo/adapter"
	"github.com/metacubex/mihomo/common/convert"
	"github.com/metacubex/mihomo/common/utils"
	"github.com/metacubex/mihomo/component/ca"
	C "github.com/metacubex/mihomo/constant"
	"github.com/metacubex/mihomo/tunnel"
)

const (
	defaultDelayTimeout = 5 * time.Second
	maxDelayTimeout     = 120 * time.Second
	minDelayTimeout     = 100 * time.Millisecond

	// maxProbeBytes caps how much of the test response is read. A probe URL
	// with a body yields a throughput estimate; the default 204 has none.
	maxProbeBytes = 64 << 10
)

// DelayOptions configures a latency test.
type DelayOptions struct {
	URL       string
	TimeoutMS int
	Expected  string
}

// DelayResult is the outcome of a latency test.
//
// Delay is the total time to the last byte (TTLB), which is what the proxy
// "delay" has always meant. TTFB is the time to the first response byte: for
// this library that is the ping (there is no ICMP probe), so no separate ping
// value exists. Bytes and SpeedBps are filled when the test URL returns a body;
// an empty 204 response leaves them zero.
type DelayResult struct {
	Name     string
	Type     string
	Server   string
	Port     int
	URL      string
	Delay    uint16
	TTFB     uint32
	TTLB     uint32
	Bytes    int64
	SpeedBps int64
}

type resolvedDelayOptions struct {
	url      string
	timeout  time.Duration
	expected utils.IntRanges[uint16]
}

func (o DelayOptions) resolve() (*resolvedDelayOptions, error) {
	url := o.URL
	if url == "" {
		url = C.DefaultTestURL
	}
	timeout := time.Duration(o.TimeoutMS) * time.Millisecond
	if timeout == 0 {
		timeout = defaultDelayTimeout
	}
	if timeout < minDelayTimeout {
		timeout = minDelayTimeout
	}
	if timeout > maxDelayTimeout {
		timeout = maxDelayTimeout
	}
	expected, err := utils.NewUnsignedRanges[uint16](o.Expected)
	if err != nil {
		return nil, fmt.Errorf("invalid expected status %q: %w", o.Expected, err)
	}
	return &resolvedDelayOptions{url: url, timeout: timeout, expected: expected}, nil
}

// probeMetrics is one HTTP probe through a proxy.
type probeMetrics struct {
	ttfb   time.Duration
	ttlb   time.Duration
	bytes  int64
	status int
}

func clampMS32(d time.Duration) uint32 {
	if d <= 0 {
		return 0
	}
	ms := d.Milliseconds()
	if ms < 1 {
		ms = 1
	}
	if ms > math.MaxUint32 {
		return math.MaxUint32
	}
	return uint32(ms)
}

func clampMS16(d time.Duration) uint16 {
	ms := clampMS32(d)
	if ms > math.MaxUint16 {
		return math.MaxUint16
	}
	return uint16(ms)
}

func (m probeMetrics) delay() uint16 { return clampMS16(m.ttlb) }

func (m probeMetrics) ttfbMS() uint32 { return clampMS32(m.ttfb) }

func (m probeMetrics) ttlbMS() uint32 { return clampMS32(m.ttlb) }

// speedBps is the body throughput: bytes over the time between the first
// byte and the last one.
func (m probeMetrics) speedBps() int64 {
	if m.bytes <= 0 {
		return 0
	}
	body := m.ttlb - m.ttfb
	bodyMS := body.Milliseconds()
	if bodyMS < 1 {
		bodyMS = 1
	}
	return m.bytes * 1000 / bodyMS
}

// TestDelay measures HTTP latency through a named proxy.
func (c *Core) TestDelay(name string, opts DelayOptions) (*DelayResult, error) {
	return c.testDelayWithContext(context.Background(), name, opts)
}

func (c *Core) testDelayWithContext(ctx context.Context, name string, opts DelayOptions) (*DelayResult, error) {
	resolved, err := opts.resolve()
	if err != nil {
		return nil, err
	}

	c.mu.RLock()
	if c.closed {
		c.mu.RUnlock()
		return nil, ErrClosed
	}
	proxy, ok := tunnel.Proxies()[name]
	c.mu.RUnlock()
	if !ok {
		return nil, fmt.Errorf("proxy %q not found", name)
	}

	testCtx, cancel := context.WithTimeout(ctx, resolved.timeout)
	defer cancel()

	metrics, err := probeProxyGet(testCtx, proxy, resolved.url, resolved.expected)
	if err != nil {
		return nil, err
	}
	return &DelayResult{
		Name:     name,
		URL:      resolved.url,
		Delay:    metrics.delay(),
		TTFB:     metrics.ttfbMS(),
		TTLB:     metrics.ttlbMS(),
		Bytes:    metrics.bytes,
		SpeedBps: metrics.speedBps(),
	}, nil
}

// TestLink parses a share link, creates a temporary proxy, measures its delay
// and closes it without touching the active configuration.
func (c *Core) TestLink(link string, opts DelayOptions) (*DelayResult, error) {
	return c.TestLinkContext(context.Background(), link, opts)
}

// TestLinkContext is TestLink with a caller-supplied cancellation context.
func (c *Core) TestLinkContext(ctx context.Context, link string, opts DelayOptions) (*DelayResult, error) {
	resolved, err := opts.resolve()
	if err != nil {
		return nil, err
	}

	mappings, err := convert.ConvertsV2Ray([]byte(link))
	if err != nil {
		return nil, fmt.Errorf("parse proxy link: %w", err)
	}
	if len(mappings) != 1 {
		return nil, fmt.Errorf("expected exactly one proxy link, got %d", len(mappings))
	}
	mapping := mappings[0]
	applyInsecureFromLink(link, mapping)
	name, _ := mapping["name"].(string)
	mapping["name"] = "mewnet-test-link"

	proxy, err := adapter.ParseProxy(mapping)
	if err != nil {
		return nil, fmt.Errorf("parse proxy: %w", err)
	}
	defer proxy.Close()

	testCtx, cancel := context.WithTimeout(ctx, resolved.timeout)
	defer cancel()

	metrics, err := probeProxyGet(testCtx, proxy, resolved.url, resolved.expected)
	if err != nil {
		return nil, err
	}

	server, _ := mapping["server"].(string)
	port, _ := toInt(mapping["port"])
	typ, _ := mapping["type"].(string)
	return &DelayResult{
		Name:     name,
		Type:     typ,
		Server:   server,
		Port:     port,
		URL:      resolved.url,
		Delay:    metrics.delay(),
		TTFB:     metrics.ttfbMS(),
		TTLB:     metrics.ttlbMS(),
		Bytes:    metrics.bytes,
		SpeedBps: metrics.speedBps(),
	}, nil
}

// TestDelayAsync schedules TestDelay and returns the task id.
func (c *Core) TestDelayAsync(name string, opts DelayOptions, cb func(TaskSnapshot)) int64 {
	return c.tasks.start(TaskKindDelay, nil, func(ctx context.Context) (any, error) {
		return c.testDelayWithContext(ctx, name, opts)
	}, cb)
}

// probeProxyGet issues a real GET through the proxy. Discovery uses GET rather
// than the adapters' HEAD-like URLTest because many broken proxies answer HEAD
// but fail a real request. It reports the time to the first byte (TTFB), the
// time to the last byte (TTLB), how many body bytes were read and the status.
func probeProxyGet(ctx context.Context, proxy C.Proxy, target string, expected utils.IntRanges[uint16]) (m probeMetrics, err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("panic during GET test: %v", r)
		}
	}()

	parsed, err := url.Parse(target)
	if err != nil {
		return m, err
	}
	port := parsed.Port()
	if port == "" {
		switch parsed.Scheme {
		case "https":
			port = "443"
		case "http":
			port = "80"
		default:
			return m, fmt.Errorf("unsupported URL scheme %q", parsed.Scheme)
		}
	}
	var metadata C.Metadata
	if err := metadata.SetRemoteAddress(net.JoinHostPort(parsed.Hostname(), port)); err != nil {
		return m, err
	}

	instance, err := proxy.DialContext(ctx, &metadata)
	if err != nil {
		return m, fmt.Errorf("delay test failed: %w", err)
	}
	defer instance.Close()

	tlsConfig, err := ca.GetTLSConfig(ca.Option{})
	if err != nil {
		return m, err
	}
	transport := &http.Transport{
		DialContext: func(context.Context, string, string) (net.Conn, error) {
			return instance, nil
		},
		MaxIdleConns:          1,
		IdleConnTimeout:       time.Second,
		TLSHandshakeTimeout:   10 * time.Second,
		ExpectContinueTimeout: time.Second,
		TLSClientConfig:       tlsConfig,
		DisableKeepAlives:     true,
	}
	client := &http.Client{
		Timeout:   30 * time.Second,
		Transport: transport,
		CheckRedirect: func(req *http.Request, via []*http.Request) error {
			return http.ErrUseLastResponse
		},
	}
	defer client.CloseIdleConnections()

	start := time.Now()
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, target, nil)
	if err != nil {
		return m, err
	}
	resp, err := client.Do(req)
	if err != nil {
		return m, fmt.Errorf("delay test failed: %w", err)
	}
	m.ttfb = time.Since(start)
	m.bytes, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, maxProbeBytes))
	m.ttlb = time.Since(start)
	_ = resp.Body.Close()
	m.status = resp.StatusCode

	status := uint16(resp.StatusCode)
	if !expected.Check(status) {
		return m, fmt.Errorf("delay test failed: unexpected status %d", resp.StatusCode)
	}
	if m.ttlb <= 0 {
		// A clock that cannot resolve one request still means "reachable".
		m.ttlb = time.Millisecond
	}
	return m, nil
}
