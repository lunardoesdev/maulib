package main

/*
#include "bridge.h"
*/
import "C"

import (
	"fmt"
	"unsafe"

	"mewnet/internal/core"
)

//export mewnet_test_delay_async
func mewnet_test_delay_async(instance *C.mewnet_instance, proxyName *C.char, options *C.mewnet_delay_options, cb C.mewnet_task_cb, user unsafe.Pointer) (task *C.mewnet_task) {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || proxyName == nil {
		return nil
	}
	coreHandle := uint64(C.mewnet_instance_get_handle(instance))
	c, err := core.Get(coreHandle)
	if err != nil {
		logError(err)
		return nil
	}
	task = C.mewnet_task_new(0, C.uint64_t(coreHandle))
	if task == nil {
		return nil
	}
	opts := parseDelayOptions(options)
	return startTask(task, cb, user, func(goCB func(core.TaskSnapshot)) int64 {
		return c.TestDelayAsync(goString(proxyName), opts, goCB)
	})
}

//export mewnet_task_poll
func mewnet_task_poll(task *C.mewnet_task) (out C.mewnet_task_result) {
	defer func() {
		if r := recover(); r != nil {
			out = C.mewnet_task_result{}
			out.state = C.MEWNET_TASK_CANCELLED
			out.error = C.MEWNET_ERROR_FAILED
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if task == nil {
		out.state = C.MEWNET_TASK_CANCELLED
		out.error = C.MEWNET_ERROR_INVALID_ARGUMENT
		return out
	}
	c, err := core.Get(uint64(C.mewnet_task_get_core(task)))
	if err != nil {
		out.state = C.MEWNET_TASK_CANCELLED
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	id := int64(C.mewnet_task_get_id(task))
	snapshot, ok := c.PollTask(id)
	if !ok {
		out.state = C.MEWNET_TASK_CANCELLED
		out.error = C.MEWNET_ERROR_NOT_FOUND
		return out
	}
	taskResultMu.Lock()
	fillTaskResult(task, &out, snapshot)
	taskResultMu.Unlock()
	return out
}

//export mewnet_task_cancel
func mewnet_task_cancel(task *C.mewnet_task) C.mewnet_error_code {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if task == nil {
		return C.MEWNET_ERROR_INVALID_ARGUMENT
	}
	c, err := core.Get(uint64(C.mewnet_task_get_core(task)))
	if err != nil {
		logError(err)
		return cErrorCode(err)
	}
	if !c.CancelTask(int64(C.mewnet_task_get_id(task))) {
		return C.MEWNET_ERROR_NOT_FOUND
	}
	return C.MEWNET_ERROR_NONE
}

//export mewnet_task_destroy
func mewnet_task_destroy(task *C.mewnet_task) {
	if task == nil {
		return
	}
	coreHandle := uint64(C.mewnet_task_get_core(task))
	taskID := int64(C.mewnet_task_get_id(task))
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
	C.mewnet_task_free(task)
}
