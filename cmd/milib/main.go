package main

/*
#include "bridge.h"
*/
import "C"

import (
	"encoding/json"
	"fmt"
	"unsafe"

	"milib/internal/core"
)

// main is required by -buildmode=c-shared. The library is driven through the
// exported milib_* functions.
func main() {}

// milib_version returns the milib version. The returned pointer is static and
// must not be freed.
//
//export milib_version
func milib_version() *C.char {
	return C.milib_version_cstr()
}

// milib_free_string releases a string returned by any milib_* function.
//
//export milib_free_string
func milib_free_string(p *C.char) {
	if p != nil {
		C.free(unsafe.Pointer(p))
	}
}

// milib_init initializes the process-wide mihomo instance.
//
// Returns 0 on success and -1 on error; on error *out_error receives an
// allocated string that must be freed with milib_free_string.
//
//export milib_init
func milib_init(homeDir *C.char, configYAML *C.char, optionsJSON *C.char, outHandle *C.uint64_t, outError **C.char) (ret C.int) {
	defer func() {
		if r := recover(); r != nil {
			if outError != nil {
				*outError = C.CString(fmt.Sprintf("panic: %v", r))
			}
			ret = -1
		}
	}()

	opts := core.InitOptions{}
	if optionsJSON != nil {
		if err := json.Unmarshal([]byte(C.GoString(optionsJSON)), &opts); err != nil {
			return initError(outError, fmt.Errorf("parse options: %w", err))
		}
	}

	home := ""
	if homeDir != nil {
		home = C.GoString(homeDir)
	}
	cfg := ""
	if configYAML != nil {
		cfg = C.GoString(configYAML)
	}

	c, handle, err := core.Init(home, cfg, opts)
	if err != nil {
		return initError(outError, err)
	}
	_ = c

	if outHandle != nil {
		*outHandle = C.uint64_t(handle)
	}
	if outError != nil {
		*outError = nil
	}
	return 0
}

func initError(outError **C.char, err error) C.int {
	if outError != nil {
		*outError = C.CString(err.Error())
	}
	return -1
}

// milib_shutdown stops the instance and releases its resources.
//
//export milib_shutdown
func milib_shutdown(h C.uint64_t) {
	defer recoverQuiet()
	_ = core.Shutdown(uint64(h))
}

// milib_add_proxy parses one or more share links (or a base64 subscription)
// and hot-swaps them into the MILIB selector group.
//
//export milib_add_proxy
func milib_add_proxy(h C.uint64_t, linkOrBlob *C.char, optionsJSON *C.char) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		return jsonError(err)
	}
	opts := core.AddOptions{}
	if optionsJSON != nil {
		if err := json.Unmarshal([]byte(C.GoString(optionsJSON)), &opts); err != nil {
			return jsonError(fmt.Errorf("parse options: %w", err))
		}
	}
	if linkOrBlob == nil {
		return jsonError(fmt.Errorf("link is required"))
	}

	result, err := c.Add(C.GoString(linkOrBlob), opts)
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(map[string]any{
		"added":    result.Added,
		"selected": result.Selected,
	})
}

// milib_remove_proxy removes a proxy from the runtime MILIB group.
//
//export milib_remove_proxy
func milib_remove_proxy(h C.uint64_t, name *C.char) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		return jsonError(err)
	}
	if name == nil {
		return jsonError(fmt.Errorf("name is required"))
	}
	info, err := c.Remove(C.GoString(name))
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(map[string]any{"removed": info})
}

// milib_list_proxies returns the members of the MILIB group.
//
//export milib_list_proxies
func milib_list_proxies(h C.uint64_t) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(c.List())
}

// milib_select changes the active member of a selector group.
//
//export milib_select
func milib_select(h C.uint64_t, group *C.char, name *C.char) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		return jsonError(err)
	}
	if name == nil {
		return jsonError(fmt.Errorf("name is required"))
	}
	groupName := ""
	if group != nil {
		groupName = C.GoString(group)
	}
	if err := c.Select(groupName, C.GoString(name)); err != nil {
		return jsonError(err)
	}
	if groupName == "" {
		groupName = core.GroupName
	}
	return jsonOK(map[string]any{"group": groupName, "selected": C.GoString(name)})
}

// milib_status returns runtime counters and listener ports.
//
//export milib_status
func milib_status(h C.uint64_t) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(c.Status())
}

func jsonOK(v map[string]any) *C.char {
	if v == nil {
		v = map[string]any{}
	}
	v["ok"] = true
	data, err := json.Marshal(v)
	if err != nil {
		return jsonError(fmt.Errorf("encode response: %w", err))
	}
	return C.CString(string(data))
}

func jsonError(err error) *C.char {
	data, marshalErr := json.Marshal(map[string]any{
		"ok":    false,
		"error": err.Error(),
	})
	if marshalErr != nil {
		return C.CString(`{"ok":false,"error":"encode error"}`)
	}
	return C.CString(string(data))
}

func recoverQuiet() {
	_ = recover()
}
