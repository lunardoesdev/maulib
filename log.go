package main

/*
#include "bridge.h"
*/
import "C"

import (
	"sync"
	"unsafe"

	mlog "github.com/metacubex/mihomo/log"
)

var (
	logMu           sync.Mutex
	logCallback     C.mewnet_log_cb
	logUser         unsafe.Pointer
	logStarted      bool
	currentLogLevel = mlog.INFO
)

//export mewnet_set_log_level
func mewnet_set_log_level(level C.mewnet_log_level) {
	mlog.SetLevel(mlog.LogLevel(int(level)))
	logMu.Lock()
	currentLogLevel = mlog.LogLevel(int(level))
	logMu.Unlock()
}

//export mewnet_set_log_callback
func mewnet_set_log_callback(cb C.mewnet_log_cb, user unsafe.Pointer) {
	logMu.Lock()
	logCallback = cb
	logUser = user
	start := !logStarted
	if start {
		logStarted = true
	}
	logMu.Unlock()

	if start {
		go relayMihomoLogs()
	}
}

func relayMihomoLogs() {
	subscription := mlog.Subscribe()
	for event := range subscription {
		logMu.Lock()
		callback := logCallback
		user := logUser
		level := currentLogLevel
		logMu.Unlock()

		if callback == nil || event.LogLevel < level {
			continue
		}
		message := C.CString(event.Payload)
		C.mewnet_call_log_cb(callback, C.mewnet_log_level(int(event.LogLevel)), message, user)
		C.free(unsafe.Pointer(message))
	}
}

func refreshLogLevel() {
	logMu.Lock()
	currentLogLevel = mlog.Level()
	logMu.Unlock()
}

func logError(err error) {
	if err == nil {
		return
	}
	mlog.Errorln("mewnet: %v", err)
}
