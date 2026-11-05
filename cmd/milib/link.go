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

// milib_link_create parses a share link and creates a SharedVPNLink: a proxy in
// the MILIB group plus an authenticated per-link SOCKS5 endpoint.
//
// Returns an opaque handle, or NULL on error (see out_error).
//
//export milib_link_create
func milib_link_create(h C.uint64_t, link *C.char, optionsJSON *C.char, outError **C.char) (out *C.milib_link) {
	defer func() {
		if r := recover(); r != nil {
			setCError(outError, fmt.Errorf("panic: %v", r))
			out = nil
		}
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		setCError(outError, err)
		return nil
	}
	if link == nil {
		setCError(outError, fmt.Errorf("link is required"))
		return nil
	}
	opts := core.LinkOptions{}
	if optionsJSON != nil {
		if err := json.Unmarshal([]byte(C.GoString(optionsJSON)), &opts); err != nil {
			setCError(outError, fmt.Errorf("parse options: %w", err))
			return nil
		}
	}

	shared, err := c.CreateLink(C.GoString(link), opts)
	if err != nil {
		setCError(outError, err)
		return nil
	}

	handle := C.milib_link_new(C.uint64_t(shared.ID), C.uint64_t(h))
	if handle == nil {
		_ = c.DestroyLink(shared.ID)
		setCError(outError, fmt.Errorf("allocate link handle"))
		return nil
	}
	if outError != nil {
		*outError = nil
	}
	return handle
}

// milib_link_destroy removes the proxy, closes the local SOCKS endpoint and
// frees the handle. Do not use the handle after this call.
//
//export milib_link_destroy
func milib_link_destroy(link *C.milib_link) (ret C.int) {
	defer func() {
		if r := recover(); r != nil {
			ret = -1
		}
	}()
	if link == nil {
		return -1
	}
	coreHandle := uint64(C.milib_link_get_core(link))
	linkID := uint64(C.milib_link_get_id(link))
	c, err := core.Get(coreHandle)
	if err != nil {
		return -1
	}
	if err := c.DestroyLink(linkID); err != nil {
		return -1
	}
	C.milib_link_free(link)
	return 0
}

// milib_link_info returns the link description, including generated auth.
//
//export milib_link_info
func milib_link_info(link *C.milib_link) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		return jsonError(err)
	}
	info, err := c.LinkInfo(id)
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(info)
}

// milib_link_list returns all live links.
//
//export milib_link_list
func milib_link_list(h C.uint64_t) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, err := core.Get(uint64(h))
	if err != nil {
		return jsonError(err)
	}
	links, err := c.ListLinks()
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(map[string]any{"links": links, "count": len(links)})
}

// milib_link_ping measures latency by passing HTTP traffic through the proxy.
//
//export milib_link_ping
func milib_link_ping(link *C.milib_link, optionsJSON *C.char) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		return jsonError(err)
	}
	opts, err := parseDelayOptions(optionsJSON)
	if err != nil {
		return jsonError(err)
	}
	result, err := c.LinkPing(id, opts)
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(result)
}

// milib_link_ping_ms measures latency and writes it as an integer number of
// milliseconds to *out_delay_ms.
//
// Returns 0 on success, -1 on error (see out_error).
//
//export milib_link_ping_ms
func milib_link_ping_ms(link *C.milib_link, optionsJSON *C.char, outDelay *C.int, outError **C.char) (ret C.int) {
	defer func() {
		if r := recover(); r != nil {
			setCError(outError, fmt.Errorf("panic: %v", r))
			ret = -1
		}
	}()

	c, id, err := resolveLink(link)
	if err != nil {
		setCError(outError, err)
		return -1
	}
	opts, err := parseDelayOptions(optionsJSON)
	if err != nil {
		setCError(outError, err)
		return -1
	}
	delay, err := c.LinkPingMS(id, opts)
	if err != nil {
		setCError(outError, err)
		return -1
	}
	if outDelay != nil {
		*outDelay = C.int(delay)
	}
	if outError != nil {
		*outError = nil
	}
	return 0
}

// milib_link_speed downloads data through the proxy and reports throughput.
//
//export milib_link_speed
func milib_link_speed(link *C.milib_link, optionsJSON *C.char) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		return jsonError(err)
	}
	opts, err := parseSpeedOptions(optionsJSON)
	if err != nil {
		return jsonError(err)
	}
	result, err := c.LinkSpeed(id, opts)
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(result)
}

// milib_link_exit_ip resolves the exit IP through the proxy.
//
//export milib_link_exit_ip
func milib_link_exit_ip(link *C.milib_link, optionsJSON *C.char) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		return jsonError(err)
	}
	opts, err := parseExitIPOptions(optionsJSON)
	if err != nil {
		return jsonError(err)
	}
	result, err := c.LinkExitIP(id, opts)
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(result)
}

// milib_link_ping_async schedules a ping task for one link.
//
//export milib_link_ping_async
func milib_link_ping_async(link *C.milib_link, optionsJSON *C.char, cb C.milib_task_cb, user unsafe.Pointer) C.int64_t {
	c, id, err := resolveLink(link)
	if err != nil {
		return asyncSetupError(cb, user, err)
	}
	opts, err := parseDelayOptions(optionsJSON)
	if err != nil {
		return asyncSetupError(cb, user, err)
	}
	return C.int64_t(c.LinkPingAsync(id, opts, goTaskCallback(cb, user)))
}

// milib_link_speed_async schedules a speed task for one link.
//
//export milib_link_speed_async
func milib_link_speed_async(link *C.milib_link, optionsJSON *C.char, cb C.milib_task_cb, user unsafe.Pointer) C.int64_t {
	c, id, err := resolveLink(link)
	if err != nil {
		return asyncSetupError(cb, user, err)
	}
	opts, err := parseSpeedOptions(optionsJSON)
	if err != nil {
		return asyncSetupError(cb, user, err)
	}
	return C.int64_t(c.LinkSpeedAsync(id, opts, goTaskCallback(cb, user)))
}

// milib_link_exit_ip_async schedules an exit-IP task for one link.
//
//export milib_link_exit_ip_async
func milib_link_exit_ip_async(link *C.milib_link, optionsJSON *C.char, cb C.milib_task_cb, user unsafe.Pointer) C.int64_t {
	c, id, err := resolveLink(link)
	if err != nil {
		return asyncSetupError(cb, user, err)
	}
	opts, err := parseExitIPOptions(optionsJSON)
	if err != nil {
		return asyncSetupError(cb, user, err)
	}
	return C.int64_t(c.LinkExitIPAsync(id, opts, goTaskCallback(cb, user)))
}

// milib_links_ping_many pings several links concurrently and waits for all of
// them. links_json is a JSON array of link ids: [1,2,3].
//
//export milib_links_ping_many
func milib_links_ping_many(h C.uint64_t, linksJSON *C.char, optionsJSON *C.char) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, err := core.Get(uint64(h))
	if err != nil {
		return jsonError(err)
	}
	if linksJSON == nil {
		return jsonError(fmt.Errorf("links is required"))
	}
	ids, err := core.ParseLinkIDs(C.GoString(linksJSON))
	if err != nil {
		return jsonError(err)
	}
	opts, err := parseDelayOptions(optionsJSON)
	if err != nil {
		return jsonError(err)
	}
	result, err := c.PingMany(ids, opts)
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(result)
}

// milib_links_ping_many_async schedules a concurrent ping of several links.
//
//export milib_links_ping_many_async
func milib_links_ping_many_async(h C.uint64_t, linksJSON *C.char, optionsJSON *C.char, cb C.milib_task_cb, user unsafe.Pointer) C.int64_t {
	c, err := core.Get(uint64(h))
	if err != nil {
		return asyncSetupError(cb, user, err)
	}
	if linksJSON == nil {
		return asyncSetupError(cb, user, fmt.Errorf("links is required"))
	}
	ids, err := core.ParseLinkIDs(C.GoString(linksJSON))
	if err != nil {
		return asyncSetupError(cb, user, err)
	}
	opts, err := parseDelayOptions(optionsJSON)
	if err != nil {
		return asyncSetupError(cb, user, err)
	}
	return C.int64_t(c.PingManyAsync(ids, opts, goTaskCallback(cb, user)))
}

func resolveLink(link *C.milib_link) (*core.Core, uint64, error) {
	if link == nil {
		return nil, 0, fmt.Errorf("invalid link handle")
	}
	coreHandle := uint64(C.milib_link_get_core(link))
	id := uint64(C.milib_link_get_id(link))
	if coreHandle == 0 || id == 0 {
		return nil, 0, fmt.Errorf("invalid link handle")
	}
	c, err := core.Get(coreHandle)
	if err != nil {
		return nil, 0, err
	}
	return c, id, nil
}

func setCError(outError **C.char, err error) {
	if outError != nil {
		*outError = C.CString(err.Error())
	}
}

func asyncSetupError(cb C.milib_task_cb, user unsafe.Pointer, err error) C.int64_t {
	if cb != nil {
		msg := C.CString(fmt.Sprintf(`{"ok":false,"error":%q}`, err.Error()))
		C.milib_call_task_cb(cb, -1, msg, user)
		C.free(unsafe.Pointer(msg))
	}
	return -1
}

func goTaskCallback(cb C.milib_task_cb, user unsafe.Pointer) func(int64, []byte) {
	if cb == nil {
		return nil
	}
	return func(id int64, result []byte) {
		msg := C.CString(string(result))
		C.milib_call_task_cb(cb, C.int64_t(id), msg, user)
		C.free(unsafe.Pointer(msg))
	}
}

func parseSpeedOptions(optionsJSON *C.char) (core.SpeedOptions, error) {
	opts := core.SpeedOptions{}
	if optionsJSON == nil {
		return opts, nil
	}
	if err := json.Unmarshal([]byte(C.GoString(optionsJSON)), &opts); err != nil {
		return opts, fmt.Errorf("parse options: %w", err)
	}
	return opts, nil
}

func parseExitIPOptions(optionsJSON *C.char) (core.ExitIPOptions, error) {
	opts := core.ExitIPOptions{}
	if optionsJSON == nil {
		return opts, nil
	}
	if err := json.Unmarshal([]byte(C.GoString(optionsJSON)), &opts); err != nil {
		return opts, fmt.Errorf("parse options: %w", err)
	}
	return opts, nil
}
