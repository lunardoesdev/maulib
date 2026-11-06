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

// Keys that describe who the proxy is (host/protocol/credentials), not how it
// is named or which transport options were used.
//
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
	hash := core.FingerprintLink(C.GoString(uri))
	dst := unsafe.Slice((*byte)(unsafe.Pointer(&result.hash[0])), 32)
	copy(dst, hash[:])
	result.ok = true
	result.error.code = C.MEWNET_ERROR_NONE
	return result
}
