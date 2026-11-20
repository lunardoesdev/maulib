package core

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"time"
)

// ProbeMetrics is what a probe through the mixed inbound measured.
//
// DelayMS is the total time to the last byte. TTFBMS is the time to the first
// byte and doubles as the ping (there is no ICMP probe). Bytes and SpeedBps are
// zero when the target returns an empty body, like the default 204.
type ProbeMetrics struct {
	DelayMS  uint32
	TTFBMS   uint32
	TTLBMS   uint32
	Bytes    int64
	SpeedBps int64
}

// ProbeHTTP fetches a URL through the mixed inbound. Unlike a direct proxy
// test (TestLink) this exercises the real local proxy path used by clients.
func (c *Core) ProbeHTTP(target string, expected, timeoutMS int) (int, ProbeMetrics, error) {
	var metrics ProbeMetrics
	c.mu.RLock()
	port := c.mixedPort
	closed := c.closed
	c.mu.RUnlock()
	if closed {
		return 0, metrics, ErrClosed
	}
	if port <= 0 {
		return 0, metrics, fmt.Errorf("mixed inbound is not listening")
	}
	if target == "" {
		target = "https://www.gstatic.com/generate_204"
	}
	timeout := time.Duration(timeoutMS) * time.Millisecond
	if timeout <= 0 {
		timeout = 3 * time.Second
	}
	proxyURL := &url.URL{Scheme: "http", Host: net.JoinHostPort("127.0.0.1", fmt.Sprint(port))}
	transport := &http.Transport{
		Proxy:               http.ProxyURL(proxyURL),
		ForceAttemptHTTP2:   false,
		DisableKeepAlives:   true,
		MaxIdleConns:        1,
		IdleConnTimeout:     time.Second,
		TLSHandshakeTimeout: timeout,
	}
	client := &http.Client{Transport: transport, Timeout: timeout}
	start := time.Now()
	req, err := http.NewRequestWithContext(context.Background(), http.MethodGet, target, nil)
	if err != nil {
		return 0, metrics, err
	}
	resp, err := client.Do(req)
	if err != nil {
		transport.CloseIdleConnections()
		return 0, metrics, err
	}
	ttfb := time.Since(start)
	bytes, _ := io.Copy(io.Discard, io.LimitReader(resp.Body, maxProbeBytes))
	ttlb := time.Since(start)
	resp.Body.Close()
	transport.CloseIdleConnections()

	if ttfb <= 0 {
		ttfb = time.Millisecond
	}
	if ttlb < ttfb {
		ttlb = ttfb
	}
	metrics = ProbeMetrics{
		DelayMS: clampMS32(ttlb),
		TTFBMS:  clampMS32(ttfb),
		TTLBMS:  clampMS32(ttlb),
		Bytes:   bytes,
	}
	if bytes > 0 {
		bodyMS := (ttlb - ttfb).Milliseconds()
		if bodyMS < 1 {
			bodyMS = 1
		}
		metrics.SpeedBps = bytes * 1000 / bodyMS
	}

	if expected > 0 {
		if resp.StatusCode != expected {
			return resp.StatusCode, metrics,
				fmt.Errorf("probe status %d, expected %d", resp.StatusCode, expected)
		}
	} else if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return resp.StatusCode, metrics, fmt.Errorf("probe status %d", resp.StatusCode)
	}
	return resp.StatusCode, metrics, nil
}
