package main

/*
#include "bridge.h"
*/
import "C"

import (
	"crypto/sha256"
	"fmt"
	"strconv"
	"strings"
	"unsafe"

	"github.com/metacubex/mihomo/common/convert"
)

// Keys that describe who the proxy is (host/protocol/credentials), not how it
// is named or which transport options were used.
var fingerprintKeys = []string{
	"type", "server", "port",
	"uuid", "password", "method", "cipher", "username", "auth",
	"alterId", "aid",
}

func normalizeFingerprintValue(v any) string {
	switch value := v.(type) {
	case nil:
		return ""
	case string:
		return strings.TrimSpace(value)
	case int:
		return strconv.Itoa(value)
	case int64:
		return strconv.FormatInt(value, 10)
	case float64:
		if value == float64(int64(value)) {
			return strconv.FormatInt(int64(value), 10)
		}
		return strconv.FormatFloat(value, 'f', -1, 64)
	case bool:
		return strconv.FormatBool(value)
	default:
		return fmt.Sprint(value)
	}
}

func fingerprintFromMapping(mapping map[string]any) string {
	var builder strings.Builder
	for _, key := range fingerprintKeys {
		value, ok := mapping[key]
		if !ok {
			continue
		}
		normalized := normalizeFingerprintValue(value)
		if normalized == "" {
			continue
		}
		if key == "type" || key == "server" || key == "method" || key == "cipher" {
			normalized = strings.ToLower(normalized)
		}
		builder.WriteString(key)
		builder.WriteByte('=')
		builder.WriteString(normalized)
		builder.WriteByte('\n')
	}
	return builder.String()
}

func fingerprintLink(link string) [32]byte {
	mappings, err := convert.ConvertsV2Ray([]byte(link))
	if err == nil && len(mappings) == 1 {
		canonical := fingerprintFromMapping(mappings[0])
		if canonical != "" {
			return sha256.Sum256([]byte(canonical))
		}
	}
	// Invalid or unusual links still need a stable identity: fall back to the
	// link body without its display-name fragment.
	base := link
	if index := strings.IndexByte(base, '#'); index >= 0 {
		base = base[:index]
	}
	return sha256.Sum256([]byte(strings.TrimSpace(base)))
}

//export mewnet_link_fingerprint
func mewnet_link_fingerprint(uri *C.char) (result C.mewnet_fingerprint_result) {
	defer func() {
		if r := recover(); r != nil {
			result = C.mewnet_fingerprint_result{}
			setCError(&result.error, fmt.Errorf("panic: %v", r))
		}
	}()
	if uri == nil {
		setCError(&result.error, fmt.Errorf("uri is required"))
		return result
	}
	hash := fingerprintLink(C.GoString(uri))
	dst := unsafe.Slice((*byte)(unsafe.Pointer(&result.hash[0])), 32)
	copy(dst, hash[:])
	result.ok = true
	result.error.code = C.MEWNET_ERROR_NONE
	return result
}
