package main

/*
#include "bridge.h"
*/
import "C"

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"sync"
	"unsafe"

	"mewnet/internal/core"
)

var taskResultMu sync.Mutex

func goString(p *C.char) string {
	if p == nil {
		return ""
	}
	return C.GoString(p)
}

func copyCString(dst []C.char, value string) {
	if len(dst) == 0 {
		return
	}
	if len(value) >= len(dst) {
		value = value[:len(dst)-1]
	}
	for i := 0; i < len(value); i++ {
		dst[i] = C.char(value[i])
	}
	dst[len(value)] = 0
}

func cErrorCode(err error) C.mewnet_error_code {
	if err == nil {
		return C.MEWNET_ERROR_NONE
	}
	if errors.Is(err, core.ErrClosed) {
		return C.MEWNET_ERROR_CLOSED
	}
	if errors.Is(err, context.Canceled) {
		return C.MEWNET_ERROR_CANCELLED
	}
	if errors.Is(err, context.DeadlineExceeded) {
		return C.MEWNET_ERROR_TIMEOUT
	}
	message := err.Error()
	switch {
	case strings.Contains(message, "not found"):
		return C.MEWNET_ERROR_NOT_FOUND
	case strings.Contains(message, "required"),
		strings.Contains(message, "invalid"),
		strings.Contains(message, "expected exactly"),
		strings.Contains(message, "empty"):
		return C.MEWNET_ERROR_INVALID_ARGUMENT
	case strings.Contains(message, "timeout"):
		return C.MEWNET_ERROR_TIMEOUT
	default:
		return C.MEWNET_ERROR_FAILED
	}
}

func setCError(out *C.mewnet_error, err error) {
	if out == nil {
		return
	}
	*out = C.mewnet_error{}
	if err == nil {
		out.code = C.MEWNET_ERROR_NONE
		return
	}
	out.code = cErrorCode(err)
	copyCString(out.message[:], err.Error())
}

func allocItems[T any](count int) (*T, error) {
	if count <= 0 {
		return nil, nil
	}
	mem := C.calloc(C.size_t(count), C.size_t(unsafe.Sizeof(*new(T))))
	if mem == nil {
		return nil, errors.New("out of memory")
	}
	return (*T)(mem), nil
}

func fillProxyInfo(dst *C.mewnet_proxy_info, src core.ProxyInfo) {
	*dst = C.mewnet_proxy_info{}
	copyCString(dst.name[:], src.Name)
	copyCString(dst.proxy_type[:], src.Type)
	copyCString(dst.server[:], src.Server)
	copyCString(dst.source[:], src.Source)
	dst.port = C.int(src.Port)
	dst.alive = C.bool(src.Alive)
	dst.delay_ms = C.uint(src.Delay)
}

func fillLinkInfo(dst *C.mewnet_link_info, src core.LinkDescription) {
	*dst = C.mewnet_link_info{}
	dst.ok = true
	dst.error = C.MEWNET_ERROR_NONE
	dst.id = C.uint64_t(src.ID)
	copyCString(dst.name[:], src.Name)
	copyCString(dst.proxy_type[:], src.Type)
	copyCString(dst.server[:], src.Server)
	copyCString(dst.uri[:], src.Link)
	copyCString(dst.socks_addr[:], src.Socks)
	copyCString(dst.socks_username[:], src.Username)
	copyCString(dst.socks_password[:], src.Password)
	dst.port = C.int(src.Port)
	dst.selected = C.bool(src.Selected)
}

func fillDelayResult(dst *C.mewnet_delay_result, src core.DelayResult) {
	*dst = C.mewnet_delay_result{}
	dst.ok = true
	dst.error = C.MEWNET_ERROR_NONE
	copyCString(dst.name[:], src.Name)
	copyCString(dst.proxy_type[:], src.Type)
	copyCString(dst.server[:], src.Server)
	copyCString(dst.url[:], src.URL)
	dst.port = C.int(src.Port)
	dst.delay_ms = C.uint(src.Delay)
	dst.ttfb_ms = C.uint(src.TTFB)
	dst.ttlb_ms = C.uint(src.TTLB)
	dst.bytes = C.ulonglong(src.Bytes)
	dst.speed_bps = C.ulonglong(src.SpeedBps)
}

func failDelayResult(dst *C.mewnet_delay_result, err error) {
	*dst = C.mewnet_delay_result{}
	dst.error = cErrorCode(err)
}

func fillPingResult(dst *C.mewnet_ping_result, src core.PingResult, err error) {
	*dst = C.mewnet_ping_result{}
	if err != nil {
		dst.error = cErrorCode(err)
		return
	}
	dst.ok = true
	dst.error = C.MEWNET_ERROR_NONE
	dst.delay_ms = C.uint(src.Delay)
	dst.ttfb_ms = C.uint(src.TTFB)
	dst.ttlb_ms = C.uint(src.TTLB)
	dst.bytes = C.ulonglong(src.Bytes)
	dst.speed_bps = C.ulonglong(src.SpeedBps)
}

func fillLinkTestResult(dst *C.mewnet_link_test_result, src core.DelayResult) {
	*dst = C.mewnet_link_test_result{}
	dst.ok = true
	dst.error.code = C.MEWNET_ERROR_NONE
	copyCString(dst.name[:], src.Name)
	copyCString(dst.proxy_type[:], src.Type)
	copyCString(dst.server[:], src.Server)
	dst.port = C.int(src.Port)
	dst.delay_ms = C.uint(src.Delay)
	dst.ttfb_ms = C.uint(src.TTFB)
	dst.ttlb_ms = C.uint(src.TTLB)
	dst.bytes = C.ulonglong(src.Bytes)
	dst.speed_bps = C.ulonglong(src.SpeedBps)
}

func failLinkTestResult(dst *C.mewnet_link_test_result, err error) {
	*dst = C.mewnet_link_test_result{}
	setCError(&dst.error, err)
}

func failPingResult(dst *C.mewnet_ping_result, err error) {
	*dst = C.mewnet_ping_result{}
	dst.error = cErrorCode(err)
}

func fillSpeedResult(dst *C.mewnet_speed_result, src core.SpeedResult) {
	*dst = C.mewnet_speed_result{}
	dst.ok = true
	dst.error = C.MEWNET_ERROR_NONE
	dst.status = C.int(src.Status)
	dst.bytes = C.longlong(src.Bytes)
	dst.seconds = C.double(src.Seconds)
	dst.bytes_per_second = C.double(src.BytesPerSecond)
	dst.mbps = C.double(src.MBPS)
}

func failSpeedResult(dst *C.mewnet_speed_result, err error) {
	*dst = C.mewnet_speed_result{}
	dst.error = cErrorCode(err)
}

func fillExitIPResult(dst *C.mewnet_exit_ip_result, src core.ExitIPResult) {
	*dst = C.mewnet_exit_ip_result{}
	dst.ok = true
	dst.error = C.MEWNET_ERROR_NONE
	dst.status = C.int(src.Status)
	dst.valid = C.bool(src.Valid)
	copyCString(dst.ip[:], src.IP)
}

func failExitIPResult(dst *C.mewnet_exit_ip_result, err error) {
	*dst = C.mewnet_exit_ip_result{}
	dst.error = cErrorCode(err)
}

func fillStatus(dst *C.mewnet_status_info, src core.StatusInfo) {
	*dst = C.mewnet_status_info{}
	dst.ok = true
	dst.error = C.MEWNET_ERROR_NONE
	copyCString(dst.version[:], src.Version)
	copyCString(dst.mihomo_version[:], src.MihomoVersion)
	copyCString(dst.mode[:], src.Mode)
	copyCString(dst.selected[:], src.Selected)
	dst.running = C.bool(src.Running)
	dst.mixed_port = C.int(src.MixedPort)
	dst.socks_port = C.int(src.SocksPort)
	dst.http_port = C.int(src.HTTPPort)
	dst.proxies = C.int(src.Proxies)
	dst.goroutines = C.uint(src.Goroutines)
	dst.rss_bytes = C.ulonglong(src.RSSBytes)
	dst.uptime_ms = C.ulonglong(src.UptimeMS)
}

func cTaskKind(kind core.TaskKind) C.mewnet_task_kind {
	switch kind {
	case core.TaskKindDelay, core.TaskKindTestLink:
		return C.MEWNET_TASK_DELAY
	case core.TaskKindLinkPing:
		return C.MEWNET_TASK_PING
	case core.TaskKindLinkSpeed:
		return C.MEWNET_TASK_SPEED
	case core.TaskKindLinkExitIP:
		return C.MEWNET_TASK_EXIT_IP
	case core.TaskKindPingMany:
		return C.MEWNET_TASK_PING_MANY
	default:
		return C.MEWNET_TASK_NONE
	}
}

func cTaskState(status core.TaskStatus) C.mewnet_task_state {
	switch status {
	case core.TaskDone:
		return C.MEWNET_TASK_DONE
	case core.TaskCancelled:
		return C.MEWNET_TASK_CANCELLED
	default:
		return C.MEWNET_TASK_RUNNING
	}
}

// cacheTaskPings replaces the task-owned ping array. taskResultMu must be held.
func cacheTaskPings(task *C.mewnet_task, outcomes []core.PingOutcome) error {
	if task == nil {
		return errors.New("invalid task handle")
	}
	if task.pings != nil {
		C.free(task.pings)
		task.pings = nil
	}
	task.ping_count = 0
	if len(outcomes) == 0 {
		return nil
	}
	items, err := allocItems[C.mewnet_ping_result](len(outcomes))
	if err != nil {
		return err
	}
	slice := unsafe.Slice(items, len(outcomes))
	for i, outcome := range outcomes {
		fillPingResult(&slice[i], core.PingResult{Delay: outcome.Delay}, outcome.Err)
	}
	task.pings = unsafe.Pointer(items)
	task.ping_count = C.size_t(len(outcomes))
	return nil
}

// fillTaskResult converts a snapshot. taskResultMu must be held.
func fillTaskResult(task *C.mewnet_task, dst *C.mewnet_task_result, snapshot core.TaskSnapshot) {
	*dst = C.mewnet_task_result{}
	dst.kind = cTaskKind(snapshot.Kind)
	dst.state = cTaskState(snapshot.Status)

	if snapshot.Status == core.TaskRunning {
		dst.error = C.MEWNET_ERROR_NONE
		return
	}

	if snapshot.Status == core.TaskCancelled && snapshot.Err == nil {
		dst.state = C.MEWNET_TASK_CANCELLED
		dst.error = C.MEWNET_ERROR_CANCELLED
		return
	}
	if snapshot.Err != nil {
		dst.error = cErrorCode(snapshot.Err)
		logError(snapshot.Err)
		return
	}

	dst.error = C.MEWNET_ERROR_NONE
	switch snapshot.Kind {
	case core.TaskKindDelay, core.TaskKindTestLink:
		if value, ok := snapshot.Value.(*core.DelayResult); ok && value != nil {
			fillDelayResult(&dst.delay, *value)
		} else {
			dst.error = C.MEWNET_ERROR_FAILED
		}
	case core.TaskKindLinkPing:
		if value, ok := snapshot.Value.(*core.PingResult); ok && value != nil {
			fillPingResult(&dst.ping, *value, nil)
		} else {
			dst.error = C.MEWNET_ERROR_FAILED
		}
	case core.TaskKindLinkSpeed:
		if value, ok := snapshot.Value.(*core.SpeedResult); ok && value != nil {
			fillSpeedResult(&dst.speed, *value)
		} else {
			dst.error = C.MEWNET_ERROR_FAILED
		}
	case core.TaskKindLinkExitIP:
		if value, ok := snapshot.Value.(*core.ExitIPResult); ok && value != nil {
			fillExitIPResult(&dst.exit_ip, *value)
		} else {
			dst.error = C.MEWNET_ERROR_FAILED
		}
	case core.TaskKindPingMany:
		value, ok := snapshot.Value.(*core.PingManyResult)
		if !ok || value == nil {
			dst.error = C.MEWNET_ERROR_FAILED
			return
		}
		if err := cacheTaskPings(task, value.Outcomes); err != nil {
			dst.error = cErrorCode(err)
			logError(err)
			return
		}
		if task != nil && task.pings != nil {
			dst.pings = (*C.mewnet_ping_result)(task.pings)
			dst.ping_count = task.ping_count
		}
	}
}

func emitTaskCallback(task *C.mewnet_task, cb C.mewnet_task_cb, user unsafe.Pointer, snapshot core.TaskSnapshot) {
	if cb == nil {
		return
	}
	var result C.mewnet_task_result
	taskResultMu.Lock()
	fillTaskResult(task, &result, snapshot)
	taskResultMu.Unlock()
	C.mewnet_call_task_cb(cb, &result, user)
}

func startTask(task *C.mewnet_task, cb C.mewnet_task_cb, user unsafe.Pointer, start func(func(core.TaskSnapshot)) int64) *C.mewnet_task {
	var goCB func(core.TaskSnapshot)
	if cb != nil {
		goCB = func(snapshot core.TaskSnapshot) {
			emitTaskCallback(task, cb, user, snapshot)
		}
	}
	id := start(goCB)
	if id <= 0 {
		C.mewnet_task_free(task)
		return nil
	}
	task.id = C.uint64_t(id)
	return task
}

func parseDelayOptions(options *C.mewnet_delay_options) core.DelayOptions {
	if options == nil {
		return core.DelayOptions{}
	}
	return core.DelayOptions{
		URL:       goString(options.url),
		TimeoutMS: int(options.timeout_ms),
		Expected:  goString(options.expected),
	}
}

func parseSpeedOptions(options *C.mewnet_speed_options) core.SpeedOptions {
	if options == nil {
		return core.SpeedOptions{}
	}
	return core.SpeedOptions{
		URL:       goString(options.url),
		Bytes:     int64(options.bytes),
		TimeoutMS: int(options.timeout_ms),
	}
}

func parseExitIPOptions(options *C.mewnet_exit_ip_options) core.ExitIPOptions {
	if options == nil {
		return core.ExitIPOptions{}
	}
	format := core.ExitIPFormatText
	if options.format == C.MEWNET_EXIT_IP_JSON {
		format = core.ExitIPFormatJSON
	}
	return core.ExitIPOptions{
		URL:       goString(options.url),
		Format:    format,
		TimeoutMS: int(options.timeout_ms),
	}
}

func linkIDsFromHandles(links **C.mewnet_link, count C.size_t) ([]uint64, error) {
	if count == 0 {
		return nil, fmt.Errorf("links array is empty")
	}
	if links == nil {
		return nil, fmt.Errorf("links is required")
	}
	handles := unsafe.Slice(links, int(count))
	ids := make([]uint64, len(handles))
	for i, handle := range handles {
		if handle == nil {
			ids[i] = 0
			continue
		}
		ids[i] = uint64(C.mewnet_link_get_id(handle))
	}
	return ids, nil
}
