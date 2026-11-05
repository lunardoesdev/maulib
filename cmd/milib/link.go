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

//export milib_link_options_default
func milib_link_options_default() (out C.milib_link_options) {
	out.local_proxy = true
	return out
}

//export milib_link_create
func milib_link_create(instance *C.milib_instance, uri *C.char, options *C.milib_link_options) (result C.milib_link_result) {
	defer func() {
		if r := recover(); r != nil {
			setCError(&result.error, fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil {
		setCError(&result.error, fmt.Errorf("invalid instance"))
		return result
	}
	c, err := core.Get(uint64(C.milib_instance_get_handle(instance)))
	if err != nil {
		setCError(&result.error, err)
		logError(err)
		return result
	}
	if uri == nil {
		err = fmt.Errorf("link is required")
		setCError(&result.error, err)
		logError(err)
		return result
	}

	opts := core.LinkOptions{}
	if options != nil {
		opts.Name = goString(options.name)
		selected := bool(options.select_proxy)
		opts.Select = &selected
		localProxy := bool(options.local_proxy)
		opts.LocalProxy = &localProxy
		opts.Username = goString(options.username)
		opts.Password = goString(options.password)
		opts.ProxyPassword = goString(options.proxy_password)
	}

	shared, err := c.CreateLink(goString(uri), opts)
	if err != nil {
		setCError(&result.error, err)
		logError(err)
		return result
	}
	handle := C.milib_link_new(C.uint64_t(shared.ID), C.uint64_t(C.milib_instance_get_handle(instance)))
	if handle == nil {
		_ = c.DestroyLink(shared.ID)
		err = fmt.Errorf("allocate link handle")
		setCError(&result.error, err)
		logError(err)
		return result
	}
	result.link = handle
	return result
}

//export milib_link_destroy
func milib_link_destroy(link *C.milib_link) {
	if link == nil {
		return
	}
	coreHandle := uint64(C.milib_link_get_core(link))
	linkID := uint64(C.milib_link_get_id(link))
	defer C.milib_link_free(link)

	c, err := core.Get(coreHandle)
	if err != nil {
		logError(err)
		return
	}
	if err := c.DestroyLink(linkID); err != nil {
		logError(err)
	}
}

//export milib_link_get_info
func milib_link_get_info(link *C.milib_link) (out C.milib_link_info) {
	defer func() {
		if r := recover(); r != nil {
			out = C.milib_link_info{}
			out.error = C.MILIB_ERROR_FAILED
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	info, err := c.LinkInfo(id)
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	fillLinkInfo(&out, *info)
	return out
}

//export milib_load_links
func milib_load_links(instance *C.milib_instance) (out C.milib_link_list) {
	defer func() {
		if r := recover(); r != nil {
			out = C.milib_link_list{}
			out.error = C.MILIB_ERROR_FAILED
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil {
		out.error = C.MILIB_ERROR_INVALID_ARGUMENT
		return out
	}
	c, err := core.Get(uint64(C.milib_instance_get_handle(instance)))
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	links, err := c.ListLinks()
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	items, err := allocItems[C.milib_link_info](len(links))
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	if items != nil {
		slice := unsafe.Slice(items, len(links))
		for i, link := range links {
			fillLinkInfo(&slice[i], link)
		}
	}
	out.ok = true
	out.error = C.MILIB_ERROR_NONE
	out.items = items
	out.count = C.size_t(len(links))
	return out
}

//export milib_unload_links
func milib_unload_links(list C.milib_link_list) {
	if list.items != nil {
		C.free(unsafe.Pointer(list.items))
	}
}

//export milib_link_ping
func milib_link_ping(link *C.milib_link, options *C.milib_delay_options) (out C.milib_ping_result) {
	defer func() {
		if r := recover(); r != nil {
			failPingResult(&out, fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		failPingResult(&out, err)
		logError(err)
		return out
	}
	result, err := c.LinkPing(id, parseDelayOptions(options))
	if err != nil {
		failPingResult(&out, err)
		logError(err)
		return out
	}
	fillPingResult(&out, *result, nil)
	return out
}

//export milib_speed_options_default
func milib_speed_options_default() (out C.milib_speed_options) {
	return out
}

//export milib_link_speed
func milib_link_speed(link *C.milib_link, options *C.milib_speed_options) (out C.milib_speed_result) {
	defer func() {
		if r := recover(); r != nil {
			failSpeedResult(&out, fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		failSpeedResult(&out, err)
		logError(err)
		return out
	}
	result, err := c.LinkSpeed(id, parseSpeedOptions(options))
	if err != nil {
		failSpeedResult(&out, err)
		logError(err)
		return out
	}
	fillSpeedResult(&out, *result)
	return out
}

//export milib_exit_ip_options_default
func milib_exit_ip_options_default() (out C.milib_exit_ip_options) {
	return out
}

//export milib_link_exit_ip
func milib_link_exit_ip(link *C.milib_link, options *C.milib_exit_ip_options) (out C.milib_exit_ip_result) {
	defer func() {
		if r := recover(); r != nil {
			failExitIPResult(&out, fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		failExitIPResult(&out, err)
		logError(err)
		return out
	}
	result, err := c.LinkExitIP(id, parseExitIPOptions(options))
	if err != nil {
		failExitIPResult(&out, err)
		logError(err)
		return out
	}
	fillExitIPResult(&out, *result)
	return out
}

//export milib_link_ping_many
func milib_link_ping_many(instance *C.milib_instance, links **C.milib_link, count C.size_t, options *C.milib_delay_options) (out C.milib_ping_many_result) {
	defer func() {
		if r := recover(); r != nil {
			out = C.milib_ping_many_result{}
			out.error = C.MILIB_ERROR_FAILED
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil {
		out.error = C.MILIB_ERROR_INVALID_ARGUMENT
		return out
	}
	c, err := core.Get(uint64(C.milib_instance_get_handle(instance)))
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	ids, err := linkIDsFromHandles(links, count)
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	result, err := c.PingMany(ids, parseDelayOptions(options))
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	items, err := allocItems[C.milib_ping_result](len(result.Outcomes))
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	if items != nil {
		slice := unsafe.Slice(items, len(result.Outcomes))
		for i, outcome := range result.Outcomes {
			fillPingResult(&slice[i], core.PingResult{Delay: outcome.Delay}, outcome.Err)
		}
	}
	out.ok = true
	out.error = C.MILIB_ERROR_NONE
	out.items = items
	out.count = C.size_t(len(result.Outcomes))
	return out
}

//export milib_unload_ping_many
func milib_unload_ping_many(result C.milib_ping_many_result) {
	if result.items != nil {
		C.free(unsafe.Pointer(result.items))
	}
}

//export milib_link_ping_async
func milib_link_ping_async(link *C.milib_link, options *C.milib_delay_options, cb C.milib_task_cb, user unsafe.Pointer) (task *C.milib_task) {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		logError(err)
		return nil
	}
	task = C.milib_task_new(0, C.uint64_t(C.milib_link_get_core(link)))
	if task == nil {
		return nil
	}
	opts := parseDelayOptions(options)
	return startTask(task, cb, user, func(goCB func(core.TaskSnapshot)) int64 {
		return c.LinkPingAsync(id, opts, goCB)
	})
}

//export milib_link_speed_async
func milib_link_speed_async(link *C.milib_link, options *C.milib_speed_options, cb C.milib_task_cb, user unsafe.Pointer) (task *C.milib_task) {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		logError(err)
		return nil
	}
	task = C.milib_task_new(0, C.uint64_t(C.milib_link_get_core(link)))
	if task == nil {
		return nil
	}
	opts := parseSpeedOptions(options)
	return startTask(task, cb, user, func(goCB func(core.TaskSnapshot)) int64 {
		return c.LinkSpeedAsync(id, opts, goCB)
	})
}

//export milib_link_exit_ip_async
func milib_link_exit_ip_async(link *C.milib_link, options *C.milib_exit_ip_options, cb C.milib_task_cb, user unsafe.Pointer) (task *C.milib_task) {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	c, id, err := resolveLink(link)
	if err != nil {
		logError(err)
		return nil
	}
	task = C.milib_task_new(0, C.uint64_t(C.milib_link_get_core(link)))
	if task == nil {
		return nil
	}
	opts := parseExitIPOptions(options)
	return startTask(task, cb, user, func(goCB func(core.TaskSnapshot)) int64 {
		return c.LinkExitIPAsync(id, opts, goCB)
	})
}

//export milib_link_ping_many_async
func milib_link_ping_many_async(instance *C.milib_instance, links **C.milib_link, count C.size_t, options *C.milib_delay_options, cb C.milib_task_cb, user unsafe.Pointer) (task *C.milib_task) {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil {
		return nil
	}
	c, err := core.Get(uint64(C.milib_instance_get_handle(instance)))
	if err != nil {
		logError(err)
		return nil
	}
	ids, err := linkIDsFromHandles(links, count)
	if err != nil {
		logError(err)
		return nil
	}
	task = C.milib_task_new(0, C.uint64_t(C.milib_instance_get_handle(instance)))
	if task == nil {
		return nil
	}
	opts := parseDelayOptions(options)
	return startTask(task, cb, user, func(goCB func(core.TaskSnapshot)) int64 {
		return c.PingManyAsync(ids, opts, goCB)
	})
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
