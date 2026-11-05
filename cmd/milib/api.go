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

// milib_test_delay measures HTTP latency through a named proxy.
//
//export milib_test_delay
func milib_test_delay(h C.uint64_t, name *C.char, optionsJSON *C.char) (out *C.char) {
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
	opts, err := parseDelayOptions(optionsJSON)
	if err != nil {
		return jsonError(err)
	}
	result, err := c.TestDelay(C.GoString(name), opts)
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(result)
}

// milib_test_link parses a share link, creates a temporary proxy, tests it and
// closes it without changing the active configuration.
//
//export milib_test_link
func milib_test_link(h C.uint64_t, link *C.char, optionsJSON *C.char) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		return jsonError(err)
	}
	if link == nil {
		return jsonError(fmt.Errorf("link is required"))
	}
	opts, err := parseDelayOptions(optionsJSON)
	if err != nil {
		return jsonError(err)
	}
	result, err := c.TestLink(C.GoString(link), opts)
	if err != nil {
		return jsonError(err)
	}
	return jsonOK(result)
}

// milib_test_delay_async schedules an asynchronous delay test. The callback is
// invoked from a Go goroutine with a JSON result; the string is only valid for
// the duration of the call.
//
// Returns a task id > 0, or -1 when the task could not be scheduled.
//
//export milib_test_delay_async
func milib_test_delay_async(h C.uint64_t, name *C.char, optionsJSON *C.char, cb C.milib_task_cb, user unsafe.Pointer) C.int64_t {
	defer func() {
		if r := recover(); r != nil {
			if cb != nil {
				msg := C.CString(fmt.Sprintf(`{"ok":false,"error":"panic: %v"}`, r))
				C.milib_call_task_cb(cb, -1, msg, user)
				C.free(unsafe.Pointer(msg))
			}
		}
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		if cb != nil {
			msg := C.CString(fmt.Sprintf(`{"ok":false,"error":%q}`, err.Error()))
			C.milib_call_task_cb(cb, -1, msg, user)
			C.free(unsafe.Pointer(msg))
		}
		return -1
	}
	if name == nil {
		if cb != nil {
			msg := C.CString(`{"ok":false,"error":"name is required"}`)
			C.milib_call_task_cb(cb, -1, msg, user)
			C.free(unsafe.Pointer(msg))
		}
		return -1
	}
	opts, err := parseDelayOptions(optionsJSON)
	if err != nil {
		if cb != nil {
			msg := C.CString(fmt.Sprintf(`{"ok":false,"error":%q}`, err.Error()))
			C.milib_call_task_cb(cb, -1, msg, user)
			C.free(unsafe.Pointer(msg))
		}
		return -1
	}

	var goCB func(int64, []byte)
	if cb != nil {
		goCB = func(id int64, result []byte) {
			msg := C.CString(string(result))
			C.milib_call_task_cb(cb, C.int64_t(id), msg, user)
			C.free(unsafe.Pointer(msg))
		}
	}
	return C.int64_t(c.TestDelayAsync(C.GoString(name), opts, goCB))
}

// milib_task_poll returns the current state of an async task as JSON.
//
//export milib_task_poll
func milib_task_poll(h C.uint64_t, taskID C.int64_t) (out *C.char) {
	defer func() {
		if r := recover(); r != nil {
			out = jsonError(fmt.Errorf("panic: %v", r))
		}
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		return jsonError(err)
	}
	data, ok := c.PollTask(int64(taskID))
	if !ok {
		return jsonError(fmt.Errorf("task %d not found", int64(taskID)))
	}
	return C.CString(data)
}

// milib_task_cancel cancels a running async task. Returns 0 on success.
//
//export milib_task_cancel
func milib_task_cancel(h C.uint64_t, taskID C.int64_t) C.int {
	defer func() {
		_ = recover()
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		return -1
	}
	if !c.CancelTask(int64(taskID)) {
		return -1
	}
	return 0
}

// milib_task_free cancels (if needed) and forgets a task. Returns 0 on success.
//
//export milib_task_free
func milib_task_free(h C.uint64_t, taskID C.int64_t) C.int {
	defer func() {
		_ = recover()
	}()

	c, err := core.Get(uint64(h))
	if err != nil {
		return -1
	}
	if !c.FreeTask(int64(taskID)) {
		return -1
	}
	return 0
}

func parseDelayOptions(optionsJSON *C.char) (core.DelayOptions, error) {
	opts := core.DelayOptions{}
	if optionsJSON == nil {
		return opts, nil
	}
	if err := json.Unmarshal([]byte(C.GoString(optionsJSON)), &opts); err != nil {
		return opts, fmt.Errorf("parse options: %w", err)
	}
	return opts, nil
}
