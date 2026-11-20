package main

/*
#include "bridge.h"
*/
import "C"

import (
	"fmt"

	"mewnet/internal/core"
)

//export mewnet_probe
func mewnet_probe(instance *C.mewnet_instance, target *C.char, expected C.int, timeout_ms C.int) (out C.mewnet_probe_result) {
	defer func() {
		if r := recover(); r != nil {
			out = C.mewnet_probe_result{}
			setCError(&out.error, fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || target == nil {
		setCError(&out.error, fmt.Errorf("instance and target are required"))
		return out
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		setCError(&out.error, err)
		return out
	}
	status, metrics, err := c.ProbeHTTP(C.GoString(target), int(expected), int(timeout_ms))
	if err != nil {
		setCError(&out.error, err)
		return out
	}
	out.ok = true
	out.status = C.int(status)
	out.delay_ms = C.uint(metrics.DelayMS)
	out.ttfb_ms = C.uint(metrics.TTFBMS)
	out.ttlb_ms = C.uint(metrics.TTLBMS)
	out.bytes = C.ulonglong(metrics.Bytes)
	out.speed_bps = C.ulonglong(metrics.SpeedBps)
	out.error.code = C.MEWNET_ERROR_NONE
	return out
}
