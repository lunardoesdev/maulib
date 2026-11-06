package core

import (
	"context"
	"errors"
	"fmt"
	"io"
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
)

// DelayOptions configures a latency test.
type DelayOptions struct {
	URL       string
	TimeoutMS int
	Expected  string
}

// DelayResult is the outcome of a latency test.
type DelayResult struct {
	Name   string
	Type   string
	Server string
	Port   int
	URL    string
	Delay  uint16
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

	delay, err := safeURLTest(testCtx, proxy, resolved.url, resolved.expected)
	if err != nil {
		return nil, err
	}
	return &DelayResult{
		Name:  name,
		URL:   resolved.url,
		Delay: delay,
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

	delay, err := safeURLTestGet(testCtx, proxy, resolved.url, resolved.expected)
	if err != nil {
		return nil, err
	}

	server, _ := mapping["server"].(string)
	port, _ := toInt(mapping["port"])
	typ, _ := mapping["type"].(string)
	return &DelayResult{
		Name:   name,
		Type:   typ,
		Server: server,
		Port:   port,
		URL:    resolved.url,
		Delay:  delay,
	}, nil
}

// TestDelayAsync schedules TestDelay and returns the task id.
func (c *Core) TestDelayAsync(name string, opts DelayOptions, cb func(TaskSnapshot)) int64 {
	return c.tasks.start(TaskKindDelay, nil, func(ctx context.Context) (any, error) {
		return c.testDelayWithContext(ctx, name, opts)
	}, cb)
}

// safeURLTestGet is the GET variant of safeURLTest. Discovery uses it because
// many broken proxies answer HEAD but fail a real GET/TLS request.
func safeURLTestGet(ctx context.Context, proxy C.Proxy, target string, expected utils.IntRanges[uint16]) (delay uint16, err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("panic during GET test: %v", r)
		}
	}()

	parsed, err := url.Parse(target)
	if err != nil {
		return 0, err
	}
	port := parsed.Port()
	if port == "" {
		switch parsed.Scheme {
		case "https":
			port = "443"
		case "http":
			port = "80"
		default:
			return 0, fmt.Errorf("unsupported URL scheme %q", parsed.Scheme)
		}
	}
	var metadata C.Metadata
	if err := metadata.SetRemoteAddress(net.JoinHostPort(parsed.Hostname(), port)); err != nil {
		return 0, err
	}

	instance, err := proxy.DialContext(ctx, &metadata)
	if err != nil {
		return 0, fmt.Errorf("delay test failed: %w", err)
	}
	defer instance.Close()

	tlsConfig, err := ca.GetTLSConfig(ca.Option{})
	if err != nil {
		return 0, err
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
		return 0, err
	}
	resp, err := client.Do(req)
	if err != nil {
		return 0, fmt.Errorf("delay test failed: %w", err)
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	resp.Body.Close()
	elapsed := time.Since(start)
	if elapsed <= 0 {
		elapsed = time.Millisecond
	}
	if elapsed > time.Duration(^uint16(0))*time.Millisecond {
		elapsed = time.Duration(^uint16(0)) * time.Millisecond
	}
	status := uint16(resp.StatusCode)
	if !expected.Check(status) {
		return 0, fmt.Errorf("delay test failed: unexpected status %d", resp.StatusCode)
	}
	return uint16(elapsed / time.Millisecond), nil
}

func safeURLTest(ctx context.Context, proxy C.Proxy, url string, expected utils.IntRanges[uint16]) (delay uint16, err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("panic during delay test: %v", r)
		}
	}()

	type outcome struct {
		delay uint16
		err   error
	}
	// Some adapters do not honor cancellation while dialing. Run the test in a
	// goroutine so the caller (discovery/health) always gets its timeout back.
	result := make(chan outcome, 1)
	go func() {
		delay, err := proxy.URLTest(ctx, url, expected)
		result <- outcome{delay: delay, err: err}
	}()

	select {
	case <-ctx.Done():
		return 0, ctx.Err()
	case value := <-result:
		delay, err = value.delay, value.err
	}
	if err != nil {
		if errors.Is(err, context.Canceled) {
			return 0, err
		}
		return 0, fmt.Errorf("delay test failed: %w", err)
	}
	if delay == 0 {
		return 0, errors.New("delay test failed: zero delay")
	}
	return delay, nil
}
