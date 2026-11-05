package core

import (
	"errors"
	"fmt"
	"net"
	"strings"

	"gopkg.in/yaml.v3"
)

// Version is the milib library version reported by milib_version().
const Version = "0.1.0"

// GroupName is the selector group managed by milib.
const GroupName = "MILIB"

const defaultConfigYAML = `
allow-lan: false
bind-address: 127.0.0.1
mode: rule
log-level: warning
ipv6: false
proxies: []
proxy-groups:
  - name: MILIB
    type: select
    proxies:
      - DIRECT
rules:
  - MATCH,MILIB
`

// InitOptions configures milib_init.
//
// MixedPort semantics:
//   - nil: use `mixed-port` from the YAML config, otherwise pick a free port;
//   - -1: disable the mixed inbound;
//   - 0: pick a free port;
//   - >0: bind exactly this port.
type InitOptions struct {
	MixedPort *int
	LogLevel  string
}

type preparedConfig struct {
	YAML      []byte
	MixedPort int
}

// prepareConfig injects the milib group, rule and inbound options into the
// user supplied YAML and returns the effective configuration.
func prepareConfig(userYAML string, opts InitOptions) (*preparedConfig, error) {
	if strings.TrimSpace(userYAML) == "" {
		userYAML = defaultConfigYAML
	}

	var cfg map[string]any
	if err := yaml.Unmarshal([]byte(userYAML), &cfg); err != nil {
		return nil, fmt.Errorf("parse config yaml: %w", err)
	}
	if cfg == nil {
		cfg = map[string]any{}
	}

	if _, ok := cfg["mode"]; !ok {
		cfg["mode"] = "rule"
	}
	if opts.LogLevel != "" {
		cfg["log-level"] = opts.LogLevel
	} else if _, ok := cfg["log-level"]; !ok {
		cfg["log-level"] = "warning"
	}
	if _, ok := cfg["bind-address"]; !ok {
		cfg["bind-address"] = "127.0.0.1"
	}
	if _, ok := cfg["allow-lan"]; !ok {
		cfg["allow-lan"] = false
	}

	mixedPort, err := resolveMixedPort(cfg, opts.MixedPort)
	if err != nil {
		return nil, err
	}

	groups, err := listOfMaps(cfg["proxy-groups"])
	if err != nil {
		return nil, fmt.Errorf("proxy-groups: %w", err)
	}

	foundGroup := false
	for _, group := range groups {
		name, _ := group["name"].(string)
		if name != GroupName {
			continue
		}
		foundGroup = true
		if typ, _ := group["type"].(string); typ != "" && !strings.EqualFold(typ, "select") {
			return nil, fmt.Errorf("proxy group %s must have type select, got %q", GroupName, typ)
		}
		group["type"] = "select"
		proxies, err := listOfStrings(group["proxies"])
		if err != nil {
			return nil, fmt.Errorf("proxy group %s proxies: %w", GroupName, err)
		}
		if len(proxies) == 0 && group["use"] == nil && group["include-all"] != true {
			group["proxies"] = []any{"DIRECT"}
		}
	}
	if !foundGroup {
		groups = append(groups, map[string]any{
			"name":    GroupName,
			"type":    "select",
			"proxies": []any{"DIRECT"},
		})
	}
	cfg["proxy-groups"] = mapsToList(groups)

	rules, err := listOfStrings(cfg["rules"])
	if err != nil {
		return nil, fmt.Errorf("rules: %w", err)
	}
	hasMatch := false
	for _, rule := range rules {
		if strings.HasPrefix(strings.ToLower(strings.TrimSpace(rule)), "match,") {
			hasMatch = true
			break
		}
	}
	if !hasMatch {
		rules = append(rules, "MATCH,"+GroupName)
	}
	cfg["rules"] = stringsToList(rules)

	cfg["mixed-port"] = mixedPort

	out, err := yaml.Marshal(cfg)
	if err != nil {
		return nil, fmt.Errorf("serialize effective config: %w", err)
	}
	return &preparedConfig{YAML: out, MixedPort: mixedPort}, nil
}

func resolveMixedPort(cfg map[string]any, requested *int) (int, error) {
	yamlPort := 0
	if v, ok := cfg["mixed-port"]; ok {
		p, err := toInt(v)
		if err != nil {
			return 0, fmt.Errorf("mixed-port: %w", err)
		}
		yamlPort = p
	}

	port := 0
	switch {
	case requested == nil && yamlPort > 0:
		port = yamlPort
	case requested != nil && *requested > 0:
		port = *requested
	case requested != nil && *requested == -1:
		return 0, nil
	}

	if port == 0 {
		free, err := freePort()
		if err != nil {
			return 0, fmt.Errorf("pick free mixed-port: %w", err)
		}
		port = free
	} else if err := checkPortFree(port); err != nil {
		return 0, err
	}

	return port, nil
}

func freePort() (int, error) {
	l, err := net.Listen("tcp4", "127.0.0.1:0")
	if err != nil {
		return 0, err
	}
	defer l.Close()
	return l.Addr().(*net.TCPAddr).Port, nil
}

func checkPortFree(port int) error {
	l, err := net.Listen("tcp4", fmt.Sprintf("127.0.0.1:%d", port))
	if err != nil {
		return fmt.Errorf("mixed-port %d is not available: %w", port, err)
	}
	return l.Close()
}

func toInt(v any) (int, error) {
	switch n := v.(type) {
	case int:
		return n, nil
	case int64:
		return int(n), nil
	case uint64:
		return int(n), nil
	case float64:
		return int(n), nil
	case string:
		var p int
		if _, err := fmt.Sscanf(strings.TrimSpace(n), "%d", &p); err != nil {
			return 0, errors.New("not an integer")
		}
		return p, nil
	default:
		return 0, fmt.Errorf("unexpected type %T", v)
	}
}

func listOfStrings(v any) ([]string, error) {
	switch t := v.(type) {
	case nil:
		return nil, nil
	case []string:
		return append([]string(nil), t...), nil
	case []any:
		out := make([]string, 0, len(t))
		for _, item := range t {
			s, ok := item.(string)
			if !ok {
				return nil, fmt.Errorf("expected string, got %T", item)
			}
			out = append(out, s)
		}
		return out, nil
	default:
		return nil, fmt.Errorf("expected list, got %T", v)
	}
}

func listOfMaps(v any) ([]map[string]any, error) {
	switch t := v.(type) {
	case nil:
		return nil, nil
	case []map[string]any:
		out := make([]map[string]any, len(t))
		copy(out, t)
		return out, nil
	case []any:
		out := make([]map[string]any, 0, len(t))
		for _, item := range t {
			m, ok := item.(map[string]any)
			if !ok {
				return nil, fmt.Errorf("expected mapping, got %T", item)
			}
			out = append(out, m)
		}
		return out, nil
	default:
		return nil, fmt.Errorf("expected list, got %T", v)
	}
}

func stringsToList(in []string) []any {
	out := make([]any, len(in))
	for i, s := range in {
		out[i] = s
	}
	return out
}

func mapsToList(in []map[string]any) []any {
	out := make([]any, len(in))
	for i, m := range in {
		out[i] = m
	}
	return out
}
