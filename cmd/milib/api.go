package main

/*
#include "bridge.h"
*/
import "C"

import (
	"fmt"
	"unsafe"

	"milib/internal/core"
)

//export milib_test_delay_async
func milib_test_delay_async(instance *C.milib_instance, proxyName *C.char, options *C.milib_delay_options, cb C.milib_task_cb, user unsafe.Pointer) (task *C.milib_task) {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || proxyName == nil {
		return nil
	}
	coreHandle := uint64(C.milib_instance_get_handle(instance))
	c, err := core.Get(coreHandle)
	if err != nil {
		logError(err)
		return nil
	}
	task = C.milib_task_new(0, C.uint64_t(coreHandle))
	if task == nil {
		return nil
	}
	opts := parseDelayOptions(options)
	return startTask(task, cb, user, func(goCB func(core.TaskSnapshot)) int64 {
		return c.TestDelayAsync(goString(proxyName), opts, goCB)
	})
}

//export milib_task_poll
func milib_task_poll(task *C.milib_task) (out C.milib_task_result) {
	defer func() {
		if r := recover(); r != nil {
			out = C.milib_task_result{}
			out.state = C.MILIB_TASK_CANCELLED
			out.error = C.MILIB_ERROR_FAILED
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if task == nil {
		out.state = C.MILIB_TASK_CANCELLED
		out.error = C.MILIB_ERROR_INVALID_ARGUMENT
		return out
	}
	c, err := core.Get(uint64(C.milib_task_get_core(task)))
	if err != nil {
		out.state = C.MILIB_TASK_CANCELLED
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	id := int64(C.milib_task_get_id(task))
	snapshot, ok := c.PollTask(id)
	if !ok {
		out.state = C.MILIB_TASK_CANCELLED
		out.error = C.MILIB_ERROR_NOT_FOUND
		return out
	}
	taskResultMu.Lock()
	fillTaskResult(task, &out, snapshot)
	taskResultMu.Unlock()
	return out
}

//export milib_task_cancel
func milib_task_cancel(task *C.milib_task) C.milib_error_code {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if task == nil {
		return C.MILIB_ERROR_INVALID_ARGUMENT
	}
	c, err := core.Get(uint64(C.milib_task_get_core(task)))
	if err != nil {
		logError(err)
		return cErrorCode(err)
	}
	if !c.CancelTask(int64(C.milib_task_get_id(task))) {
		return C.MILIB_ERROR_NOT_FOUND
	}
	return C.MILIB_ERROR_NONE
}

//export milib_task_destroy
func milib_task_destroy(task *C.milib_task) {
	if task == nil {
		return
	}
	coreHandle := uint64(C.milib_task_get_core(task))
	taskID := int64(C.milib_task_get_id(task))
	if c, err := core.Get(coreHandle); err == nil {
		c.FreeTask(taskID)
	}

	taskResultMu.Lock()
	if task.pings != nil {
		C.free(task.pings)
		task.pings = nil
		task.ping_count = 0
	}
	taskResultMu.Unlock()
	C.milib_task_free(task)
}
