package core

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/metacubex/mihomo/config"
	C "github.com/metacubex/mihomo/constant"
	P "github.com/metacubex/mihomo/constant/provider"
	"github.com/metacubex/mihomo/hub"
	"github.com/metacubex/mihomo/hub/executor"
	"github.com/metacubex/mihomo/listener"
	"github.com/metacubex/mihomo/log"
	"github.com/metacubex/mihomo/tunnel"
	"github.com/metacubex/mihomo/tunnel/statistic"

	"gopkg.in/yaml.v3"
)

var (
	ErrInvalidHandle = errors.New("invalid handle")
	ErrClosed        = errors.New("core is shut down")

	lifecycleMu sync.Mutex

	registryMu   sync.Mutex
	active       *Core
	activeHandle uint64
	nextHandle   uint64 = 1
)

type proxyMeta struct {
	Type   string
	Server string
	Port   int
}

type entry struct {
	Name    string
	Type    string
	Server  string
	Port    int
	Mapping map[string]any
	Source  string
	AddedAt time.Time
}

// Core owns the single mihomo instance of the process.
type Core struct {
	mu sync.RWMutex

	homeDir     string
	startedAt   time.Time
	baseProxies []map[string]any
	baseGroups  []map[string]any
	baseMeta    map[string]proxyMeta
	excluded    map[string]bool
	entries     map[string]*entry
	order       []string
	links       map[uint64]*SharedLink
	nextLinkID  uint64
	mixedPort   int
	selected    string
	tasks       *taskStore
	closed      bool
}

type rawSkeleton struct {
	Proxies     []map[string]any `yaml:"proxies"`
	ProxyGroups []map[string]any `yaml:"proxy-groups"`
}

// Init initializes the global mihomo instance and returns its handle.
func Init(homeDir, configYAML string, opts InitOptions) (*Core, uint64, error) {
	lifecycleMu.Lock()
	defer lifecycleMu.Unlock()

	registryMu.Lock()
	alreadyActive := active != nil
	registryMu.Unlock()
	if alreadyActive {
		return nil, 0, errors.New("milib is already initialized")
	}

	if strings.TrimSpace(homeDir) == "" {
		homeDir = filepath.Join(os.TempDir(), "milib-home")
	}
	homeAbs, err := filepath.Abs(homeDir)
	if err != nil {
		return nil, 0, fmt.Errorf("resolve home dir: %w", err)
	}
	if err := os.MkdirAll(homeAbs, 0o755); err != nil {
		return nil, 0, fmt.Errorf("create home dir: %w", err)
	}

	C.SetHomeDir(homeAbs)

	// Apply the requested log level before config.Parse(), which logs
	// startup messages before hub.ApplyConfig can set the level.
	if opts.LogLevel != "" {
		if level, ok := log.LogLevelMapping[strings.ToLower(opts.LogLevel)]; ok {
			log.SetLevel(level)
		}
	}

	prep, err := prepareConfig(configYAML, opts)
	if err != nil {
		return nil, 0, err
	}

	cfg, err := config.Parse(prep.YAML)
	if err != nil {
		return nil, 0, fmt.Errorf("parse config: %w", err)
	}

	hub.ApplyConfig(cfg)

	ports := listener.GetPorts()
	if prep.MixedPort > 0 && ports.MixedPort == 0 {
		stopInbounds()
		return nil, 0, fmt.Errorf("mixed inbound failed to listen on port %d", prep.MixedPort)
	}

	var skeleton rawSkeleton
	if err := yaml.Unmarshal(prep.YAML, &skeleton); err != nil {
		stopInbounds()
		return nil, 0, fmt.Errorf("read effective config: %w", err)
	}

	c := &Core{
		homeDir:     homeAbs,
		startedAt:   time.Now(),
		baseProxies: skeleton.Proxies,
		baseGroups:  skeleton.ProxyGroups,
		baseMeta:    make(map[string]proxyMeta, len(skeleton.Proxies)),
		excluded:    make(map[string]bool),
		entries:     make(map[string]*entry),
		links:       make(map[uint64]*SharedLink),
		mixedPort:   ports.MixedPort,
		tasks:       newTaskStore(),
	}
	for _, mapping := range skeleton.Proxies {
		name, _ := mapping["name"].(string)
		if name == "" {
			continue
		}
		port, _ := toInt(mapping["port"])
		typ, _ := mapping["type"].(string)
		server, _ := mapping["server"].(string)
		c.baseMeta[name] = proxyMeta{Type: strings.ToUpper(typ), Server: server, Port: port}
	}
	c.selected = currentSelection()

	registryMu.Lock()
	handle := nextHandle
	nextHandle++
	active = c
	activeHandle = handle
	registryMu.Unlock()

	log.Infoln("milib %s initialized (mihomo %s), mixed-port %d", Version, C.Version, c.mixedPort)
	return c, handle, nil
}

// Shutdown stops the instance identified by handle.
func Shutdown(handle uint64) error {
	lifecycleMu.Lock()
	defer lifecycleMu.Unlock()

	c, err := get(handle)
	if err != nil {
		return err
	}
	c.stop()

	registryMu.Lock()
	if active == c {
		active = nil
		activeHandle = 0
	}
	registryMu.Unlock()
	return nil
}

// Get returns the active core for handle.
func Get(handle uint64) (*Core, error) {
	return get(handle)
}

func get(handle uint64) (*Core, error) {
	registryMu.Lock()
	defer registryMu.Unlock()
	if active == nil || activeHandle != handle {
		return nil, ErrInvalidHandle
	}
	return active, nil
}

func (c *Core) stop() {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return
	}
	c.closed = true
	c.mu.Unlock()

	if c.tasks != nil {
		c.tasks.shutdown()
	}

	c.mu.Lock()
	for id, link := range c.links {
		if link.listener != nil {
			_ = link.listener.Close()
		}
		delete(c.links, id)
	}
	c.mu.Unlock()

	stopInbounds()

	c.mu.Lock()
	proxies := tunnel.Proxies()
	providers := tunnel.Providers()
	c.mu.Unlock()

	for _, p := range proxies {
		_ = p.Close()
	}
	for _, p := range providers {
		closeProvider(p)
	}
	tunnel.UpdateProxies(map[string]C.Proxy{}, map[string]P.ProxyProvider{})

	// Close all active connections so no goroutine keeps using closed proxies.
	statistic.DefaultManager.Range(func(t statistic.Tracker) bool {
		_ = t.Close()
		return true
	})

	executor.Shutdown()
	log.Infoln("milib shut down")
}

// stopInbounds closes every inbound listener. listener.Cleanup only handles
// TUN, so the stream inbounds must be re-created with a zero port.
func stopInbounds() {
	listener.ReCreateHTTP(0, tunnel.Tunnel)
	listener.ReCreateSocks(0, tunnel.Tunnel)
	listener.ReCreateMixed(0, tunnel.Tunnel)
	listener.ReCreateRedir(0, tunnel.Tunnel)
	listener.ReCreateTProxy(0, tunnel.Tunnel)
	listener.ReCreateShadowSocks("", tunnel.Tunnel)
	listener.ReCreateVmess("", tunnel.Tunnel)
	listener.Cleanup()
}

func currentSelection() string {
	proxies := tunnel.Proxies()
	group, ok := proxies[GroupName]
	if !ok {
		return "DIRECT"
	}
	if now, ok := group.Adapter().(interface{ Now() string }); ok {
		if name := now.Now(); name != "" {
			return name
		}
	}
	return "DIRECT"
}

// Status returns the JSON object for milib_status.
func (c *Core) Status() map[string]any {
	c.mu.RLock()
	if c.closed {
		c.mu.RUnlock()
		return map[string]any{"ok": false, "error": ErrClosed.Error()}
	}
	startedAt := c.startedAt
	selected := c.selected
	members := c.groupMembersLocked()
	c.mu.RUnlock()

	ports := listener.GetPorts()
	if selected == "" {
		selected = currentSelection()
	}

	return map[string]any{
		"ok":             true,
		"version":        Version,
		"mihomo_version": C.Version,
		"running":        true,
		"mode":           tunnel.Mode().String(),
		"mixed_port":     ports.MixedPort,
		"socks_port":     ports.SocksPort,
		"http_port":      ports.Port,
		"proxies":        len(members),
		"selected":       selected,
		"goroutines":     runtime.NumGoroutine(),
		"rss_bytes":      rssBytes(),
		"uptime_ms":      time.Since(startedAt).Milliseconds(),
	}
}

func rssBytes() int64 {
	data, err := os.ReadFile("/proc/self/statm")
	if err != nil {
		return 0
	}
	fields := strings.Fields(string(data))
	if len(fields) < 2 {
		return 0
	}
	pages, err := strconv.ParseInt(fields[1], 10, 64)
	if err != nil {
		return 0
	}
	return pages * int64(os.Getpagesize())
}
