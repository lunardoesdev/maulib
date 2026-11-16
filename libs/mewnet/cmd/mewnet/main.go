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

// main is required by -buildmode=c-shared.
func main() {}

//export mewnet_abi_version
func mewnet_abi_version() C.int {
	return C.MEWNET_ABI_VERSION
}

//export mewnet_version
func mewnet_version() *C.char {
	return C.mewnet_version_cstr()
}

//export mewnet_config_default
func mewnet_config_default() (out C.mewnet_config) {
	return out
}

//export mewnet_init
func mewnet_init(config *C.mewnet_config) (result C.mewnet_init_result) {
	defer func() {
		if r := recover(); r != nil {
			setCError(&result.error, fmt.Errorf("panic: %v", r))
		}
	}()

	opts := core.InitOptions{}
	homeDir := ""
	configYAML := ""
	if config != nil {
		if config.mixed_port != 0 {
			mixedPort := int(config.mixed_port)
			opts.MixedPort = &mixedPort
		}
		opts.LogLevel = goString(config.log_level)
		homeDir = goString(config.home_dir)
		configYAML = goString(config.config_yaml)
	}

	_, handle, err := core.Init(homeDir, configYAML, opts)
	if err != nil {
		setCError(&result.error, err)
		logError(err)
		return result
	}

	refreshLogLevel()

	instance := C.mewnet_instance_new(C.uint64_t(handle))
	if instance == nil {
		_ = core.Shutdown(handle)
		err = fmt.Errorf("allocate instance handle")
		setCError(&result.error, err)
		logError(err)
		return result
	}
	result.instance = instance
	return result
}

//export mewnet_close
func mewnet_close(instance *C.mewnet_instance) {
	if instance == nil {
		return
	}
	handle := uint64(C.mewnet_instance_get_handle(instance))
	defer C.mewnet_instance_free(instance)
	if err := core.Shutdown(handle); err != nil {
		logError(err)
	}
}

//export mewnet_get_status
func mewnet_get_status(instance *C.mewnet_instance) (out C.mewnet_status_info) {
	defer func() {
		if r := recover(); r != nil {
			out = C.mewnet_status_info{}
			out.error = C.MEWNET_ERROR_FAILED
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil {
		out.error = C.MEWNET_ERROR_INVALID_ARGUMENT
		return out
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	status, err := c.Status()
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	fillStatus(&out, *status)
	return out
}

//export mewnet_load_proxies
func mewnet_load_proxies(instance *C.mewnet_instance) (out C.mewnet_proxy_list) {
	defer func() {
		if r := recover(); r != nil {
			out = C.mewnet_proxy_list{}
			out.error = C.MEWNET_ERROR_FAILED
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil {
		out.error = C.MEWNET_ERROR_INVALID_ARGUMENT
		return out
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	list, err := c.List()
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}

	items, err := allocItems[C.mewnet_proxy_info](len(list.Proxies))
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	if items != nil {
		slice := unsafe.Slice(items, len(list.Proxies))
		for i, proxy := range list.Proxies {
			fillProxyInfo(&slice[i], proxy)
		}
	}
	out.ok = true
	out.error = C.MEWNET_ERROR_NONE
	out.items = items
	out.count = C.size_t(len(list.Proxies))
	copyCString(out.selected[:], list.Selected)
	return out
}

//export mewnet_unload_proxies
func mewnet_unload_proxies(list C.mewnet_proxy_list) {
	if list.items != nil {
		C.free(unsafe.Pointer(list.items))
	}
}

//export mewnet_add_options_default
func mewnet_add_options_default() (out C.mewnet_add_options) {
	out.select_proxy = true
	return out
}

//export mewnet_add_proxy
func mewnet_add_proxy(instance *C.mewnet_instance, uriOrBlob *C.char, options *C.mewnet_add_options) C.mewnet_error_code {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil {
		return C.MEWNET_ERROR_INVALID_ARGUMENT
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		logError(err)
		return cErrorCode(err)
	}
	if uriOrBlob == nil {
		err = fmt.Errorf("link is required")
		logError(err)
		return cErrorCode(err)
	}

	opts := core.AddOptions{}
	if options != nil {
		opts.Name = goString(options.name)
		selected := bool(options.select_proxy)
		opts.Select = &selected
		opts.SkipCertVerify = bool(options.skip_cert_verify)
	}
	if _, err := c.Add(goString(uriOrBlob), opts); err != nil {
		logError(err)
		return cErrorCode(err)
	}
	return C.MEWNET_ERROR_NONE
}

//export mewnet_remove_proxy
func mewnet_remove_proxy(instance *C.mewnet_instance, name *C.char) C.mewnet_error_code {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || name == nil {
		return C.MEWNET_ERROR_INVALID_ARGUMENT
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		logError(err)
		return cErrorCode(err)
	}
	if _, err := c.Remove(goString(name)); err != nil {
		logError(err)
		return cErrorCode(err)
	}
	return C.MEWNET_ERROR_NONE
}

//export mewnet_select_proxy
func mewnet_select_proxy(instance *C.mewnet_instance, name *C.char) C.mewnet_error_code {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || name == nil {
		return C.MEWNET_ERROR_INVALID_ARGUMENT
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		logError(err)
		return cErrorCode(err)
	}
	if err := c.Select("", goString(name)); err != nil {
		logError(err)
		return cErrorCode(err)
	}
	return C.MEWNET_ERROR_NONE
}

//export mewnet_has_proxy
func mewnet_has_proxy(instance *C.mewnet_instance, name *C.char) (ok C.bool) {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
			ok = false
		}
	}()
	if instance == nil || name == nil {
		return false
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		logError(err)
		return false
	}
	return C.bool(c.Has(goString(name)))
}

//export mewnet_delay_options_default
func mewnet_delay_options_default() (out C.mewnet_delay_options) {
	return out
}

//export mewnet_test_delay
func mewnet_test_delay(instance *C.mewnet_instance, proxyName *C.char, options *C.mewnet_delay_options) (out C.mewnet_delay_result) {
	defer func() {
		if r := recover(); r != nil {
			failDelayResult(&out, fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || proxyName == nil {
		failDelayResult(&out, fmt.Errorf("invalid argument"))
		return out
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		failDelayResult(&out, err)
		logError(err)
		return out
	}
	result, err := c.TestDelay(goString(proxyName), parseDelayOptions(options))
	if err != nil {
		failDelayResult(&out, err)
		logError(err)
		return out
	}
	fillDelayResult(&out, *result)
	return out
}

//export mewnet_test_link_info
func mewnet_test_link_info(instance *C.mewnet_instance, uri *C.char, options *C.mewnet_delay_options) (out C.mewnet_link_test_result) {
	defer func() {
		if r := recover(); r != nil {
			setCError(&out.error, fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || uri == nil {
		setCError(&out.error, fmt.Errorf("invalid argument"))
		return out
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		setCError(&out.error, err)
		logError(err)
		return out
	}
	result, err := c.TestLink(goString(uri), parseDelayOptions(options))
	if err != nil {
		setCError(&out.error, err)
		logError(err)
		return out
	}
	out.ok = true
	out.error.code = C.MEWNET_ERROR_NONE
	copyCString(out.name[:], result.Name)
	copyCString(out.proxy_type[:], result.Type)
	copyCString(out.server[:], result.Server)
	out.port = C.int(result.Port)
	out.delay_ms = C.uint(result.Delay)
	return out
}

//export mewnet_test_link
func mewnet_test_link(instance *C.mewnet_instance, uri *C.char, options *C.mewnet_delay_options) (out C.mewnet_delay_result) {
	defer func() {
		if r := recover(); r != nil {
			failDelayResult(&out, fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || uri == nil {
		failDelayResult(&out, fmt.Errorf("invalid argument"))
		return out
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		failDelayResult(&out, err)
		logError(err)
		return out
	}
	result, err := c.TestLink(goString(uri), parseDelayOptions(options))
	if err != nil {
		failDelayResult(&out, err)
		logError(err)
		return out
	}
	fillDelayResult(&out, *result)
	return out
}
