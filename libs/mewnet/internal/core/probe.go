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

// ProbeHTTP fetches a URL through the mixed inbound. Unlike URLTest (HEAD to
// the proxy adapter) this exercises the real local proxy path used by clients.
func (c *Core) ProbeHTTP(target string, expected, timeoutMS int) (int, uint16, error) {
	c.mu.RLock()
	port := c.mixedPort
	closed := c.closed
	c.mu.RUnlock()
	if closed {
		return 0, 0, ErrClosed
	}
	if port <= 0 {
		return 0, 0, fmt.Errorf("mixed inbound is not listening")
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
		return 0, 0, err
	}
	resp, err := client.Do(req)
	if err != nil {
		transport.CloseIdleConnections()
		return 0, 0, err
	}
	_, _ = io.Copy(io.Discard, io.LimitReader(resp.Body, 4096))
	resp.Body.Close()
	transport.CloseIdleConnections()
	delay := time.Since(start)
	if delay <= 0 {
		delay = time.Millisecond
	}
	if delay > time.Duration(^uint16(0))*time.Millisecond {
		delay = time.Duration(^uint16(0)) * time.Millisecond
	}
	if expected > 0 {
		if resp.StatusCode != expected {
			return resp.StatusCode, uint16(delay / time.Millisecond),
				fmt.Errorf("probe status %d, expected %d", resp.StatusCode, expected)
		}
	} else if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return resp.StatusCode, uint16(delay / time.Millisecond),
			fmt.Errorf("probe status %d", resp.StatusCode)
	}
	return resp.StatusCode, uint16(delay / time.Millisecond), nil
}
