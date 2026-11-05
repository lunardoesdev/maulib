package core

import (
	"context"
	"errors"
	"fmt"
	"time"

	"github.com/metacubex/mihomo/adapter"
	"github.com/metacubex/mihomo/common/convert"
	"github.com/metacubex/mihomo/common/utils"
	C "github.com/metacubex/mihomo/constant"
	"github.com/metacubex/mihomo/tunnel"
)

const (
	defaultDelayTimeout = 5 * time.Second
	maxDelayTimeout     = 120 * time.Second
	minDelayTimeout     = 100 * time.Millisecond
)

// DelayOptions maps the JSON options accepted by delay test functions.
type DelayOptions struct {
	URL       string `json:"url"`
	TimeoutMS int    `json:"timeout_ms"`
	Expected  string `json:"expected"`
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
func (c *Core) TestDelay(name string, opts DelayOptions) (map[string]any, error) {
	result, err := c.testDelayWithContext(context.Background(), name, opts)
	if err != nil {
		return nil, err
	}
	result["ok"] = true
	return result, nil
}

func (c *Core) testDelayWithContext(ctx context.Context, name string, opts DelayOptions) (map[string]any, error) {
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
	return map[string]any{
		"name":  name,
		"delay": delay,
		"url":   resolved.url,
	}, nil
}

// TestLink parses a share link, creates a temporary proxy, measures its delay
// and closes it without touching the active configuration.
func (c *Core) TestLink(link string, opts DelayOptions) (map[string]any, error) {
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
	mapping["name"] = "milib-test-link"

	proxy, err := adapter.ParseProxy(mapping)
	if err != nil {
		return nil, fmt.Errorf("parse proxy: %w", err)
	}
	defer proxy.Close()

	ctx, cancel := context.WithTimeout(context.Background(), resolved.timeout)
	defer cancel()

	delay, err := safeURLTest(ctx, proxy, resolved.url, resolved.expected)
	if err != nil {
		return nil, err
	}

	server, _ := mapping["server"].(string)
	port, _ := toInt(mapping["port"])
	typ, _ := mapping["type"].(string)
	return map[string]any{
		"ok":     true,
		"type":   typ,
		"server": server,
		"port":   port,
		"delay":  delay,
		"url":    resolved.url,
	}, nil
}

// TestDelayAsync schedules TestDelay and returns the task id.
func (c *Core) TestDelayAsync(name string, opts DelayOptions, cb func(int64, []byte)) int64 {
	return c.tasks.start(func(ctx context.Context) (any, error) {
		return c.testDelayWithContext(ctx, name, opts)
	}, cb)
}

func safeURLTest(ctx context.Context, proxy C.Proxy, url string, expected utils.IntRanges[uint16]) (delay uint16, err error) {
	defer func() {
		if r := recover(); r != nil {
			err = fmt.Errorf("panic during delay test: %v", r)
		}
	}()

	delay, err = proxy.URLTest(ctx, url, expected)
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
