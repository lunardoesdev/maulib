// Package mewnet is the importable Go API for the mewnet core.
//
// It is a thin, idiomatic wrapper around internal/core. The C subscriptions
// and the discovery agent are separate C code built into the same shared
// library for non-Go hosts; this package exposes the Go core.
package mewnet

import (
	"context"
	"encoding/hex"
	"sync"

	"mewnet/internal/core"
)

// Version is the library version.
const Version = core.Version

// Errors returned by the core.
var (
	ErrInvalidHandle = core.ErrInvalidHandle
	ErrClosed        = core.ErrClosed
)

// Core is the underlying mihomo-backed engine. Most users should use Client,
// but Core is exported so advanced code can refer to method sets and types.
type Core = core.Core

// Type aliases keep the API close to the underlying implementation while
// still allowing an external Go module to import this package.
type (
	StatusInfo      = core.StatusInfo
	ProxyInfo       = core.ProxyInfo
	ProxyList       = core.ProxyList
	AddOptions      = core.AddOptions
	AddResult       = core.AddResult
	DelayOptions    = core.DelayOptions
	DelayResult     = core.DelayResult
	SpeedOptions    = core.SpeedOptions
	SpeedResult     = core.SpeedResult
	ExitIPOptions   = core.ExitIPOptions
	ExitIPResult    = core.ExitIPResult
	ExitIPFormat    = core.ExitIPFormat
	LinkOptions     = core.LinkOptions
	LinkDescription = core.LinkDescription
	PingResult      = core.PingResult
	PingOutcome     = core.PingOutcome
	PingManyResult  = core.PingManyResult
	TaskKind        = core.TaskKind
	TaskStatus      = core.TaskStatus
	TaskSnapshot    = core.TaskSnapshot
	SharedLink      = core.SharedLink
)

const (
	ExitIPFormatText = core.ExitIPFormatText
	ExitIPFormatJSON = core.ExitIPFormatJSON

	TaskKindUnknown    = core.TaskKindUnknown
	TaskKindDelay      = core.TaskKindDelay
	TaskKindTestLink   = core.TaskKindTestLink
	TaskKindLinkPing   = core.TaskKindLinkPing
	TaskKindLinkSpeed  = core.TaskKindLinkSpeed
	TaskKindLinkExitIP = core.TaskKindLinkExitIP
	TaskKindPingMany   = core.TaskKindPingMany

	TaskRunning   = core.TaskRunning
	TaskDone      = core.TaskDone
	TaskCancelled = core.TaskCancelled
)

// Config configures Start.
type Config struct {
	// HomeDir is the mihomo home directory. Empty means a temporary directory.
	HomeDir string
	// ConfigYAML is an optional mihomo YAML configuration.
	ConfigYAML string
	// LogLevel is one of "debug", "info", "warning", "error", "silent".
	LogLevel string
	// MixedPort controls the mixed HTTP/SOCKS inbound:
	//   -1 disable, 0 pick a free port (or use YAML), >0 bind exactly.
	MixedPort int
}

// Client owns one started mewnet core.
type Client struct {
	*Core
	handle uint64

	closeOnce sync.Once
	closeErr  error
}

// Start initializes the core and returns a client.
func Start(cfg Config) (*Client, error) {
	var port *int
	if cfg.MixedPort != 0 {
		value := cfg.MixedPort
		port = &value
	}
	c, handle, err := core.Init(cfg.HomeDir, cfg.ConfigYAML, core.InitOptions{
		MixedPort: port,
		LogLevel:  cfg.LogLevel,
	})
	if err != nil {
		return nil, err
	}
	return &Client{Core: c, handle: handle}, nil
}

// Handle returns the core handle used by the C lifecycle functions.
func (c *Client) Handle() uint64 {
	if c == nil {
		return 0
	}
	return c.handle
}

// Close shuts the core down. It is safe to call more than once.
func (c *Client) Close() error {
	if c == nil {
		return nil
	}
	c.closeOnce.Do(func() {
		c.closeErr = core.Shutdown(c.handle)
	})
	return c.closeErr
}

// Fingerprint returns the stable identity of a share link as lowercase hex.
// It is the same identity used by the subscription proxy deduplication.
func Fingerprint(link string) string {
	sum := core.FingerprintLink(link)
	return hex.EncodeToString(sum[:])
}

// FingerprintBytes returns the raw 32-byte identity of a share link.
func FingerprintBytes(link string) [32]byte {
	return core.FingerprintLink(link)
}

// TestLinksBatch checks links concurrently and waits for all of them.
func (c *Client) TestLinksBatch(ctx context.Context, links []string, opts DelayOptions, concurrency int) []LinkTest {
	return c.testLinksBatch(ctx, links, opts, concurrency, false)
}

// TestLinksBatchFirst checks links concurrently and cancels the remaining
// checks as soon as the first working link is found.
func (c *Client) TestLinksBatchFirst(ctx context.Context, links []string, opts DelayOptions, concurrency int) []LinkTest {
	return c.testLinksBatch(ctx, links, opts, concurrency, true)
}
