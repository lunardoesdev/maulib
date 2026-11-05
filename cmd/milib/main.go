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

// main is required by -buildmode=c-shared.
func main() {}

//export milib_abi_version
func milib_abi_version() C.int {
	return C.MILIB_ABI_VERSION
}

//export milib_version
func milib_version() *C.char {
	return C.milib_version_cstr()
}

//export milib_config_default
func milib_config_default() (out C.milib_config) {
	return out
}

//export milib_init
func milib_init(config *C.milib_config) (result C.milib_init_result) {
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

	instance := C.milib_instance_new(C.uint64_t(handle))
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

//export milib_close
func milib_close(instance *C.milib_instance) {
	if instance == nil {
		return
	}
	handle := uint64(C.milib_instance_get_handle(instance))
	defer C.milib_instance_free(instance)
	if err := core.Shutdown(handle); err != nil {
		logError(err)
	}
}

//export milib_get_status
func milib_get_status(instance *C.milib_instance) (out C.milib_status_info) {
	defer func() {
		if r := recover(); r != nil {
			out = C.milib_status_info{}
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
	status, err := c.Status()
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}
	fillStatus(&out, *status)
	return out
}

//export milib_load_proxies
func milib_load_proxies(instance *C.milib_instance) (out C.milib_proxy_list) {
	defer func() {
		if r := recover(); r != nil {
			out = C.milib_proxy_list{}
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
	list, err := c.List()
	if err != nil {
		out.error = cErrorCode(err)
		logError(err)
		return out
	}

	items, err := allocItems[C.milib_proxy_info](len(list.Proxies))
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
	out.error = C.MILIB_ERROR_NONE
	out.items = items
	out.count = C.size_t(len(list.Proxies))
	copyCString(out.selected[:], list.Selected)
	return out
}

//export milib_unload_proxies
func milib_unload_proxies(list C.milib_proxy_list) {
	if list.items != nil {
		C.free(unsafe.Pointer(list.items))
	}
}

//export milib_add_options_default
func milib_add_options_default() (out C.milib_add_options) {
	out.select_proxy = true
	return out
}

//export milib_add_proxy
func milib_add_proxy(instance *C.milib_instance, uriOrBlob *C.char, options *C.milib_add_options) C.milib_error_code {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil {
		return C.MILIB_ERROR_INVALID_ARGUMENT
	}
	c, err := core.Get(uint64(C.milib_instance_get_handle(instance)))
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
	return C.MILIB_ERROR_NONE
}

//export milib_remove_proxy
func milib_remove_proxy(instance *C.milib_instance, name *C.char) C.milib_error_code {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || name == nil {
		return C.MILIB_ERROR_INVALID_ARGUMENT
	}
	c, err := core.Get(uint64(C.milib_instance_get_handle(instance)))
	if err != nil {
		logError(err)
		return cErrorCode(err)
	}
	if _, err := c.Remove(goString(name)); err != nil {
		logError(err)
		return cErrorCode(err)
	}
	return C.MILIB_ERROR_NONE
}

//export milib_select_proxy
func milib_select_proxy(instance *C.milib_instance, name *C.char) C.milib_error_code {
	defer func() {
		if r := recover(); r != nil {
			logError(fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || name == nil {
		return C.MILIB_ERROR_INVALID_ARGUMENT
	}
	c, err := core.Get(uint64(C.milib_instance_get_handle(instance)))
	if err != nil {
		logError(err)
		return cErrorCode(err)
	}
	if err := c.Select("", goString(name)); err != nil {
		logError(err)
		return cErrorCode(err)
	}
	return C.MILIB_ERROR_NONE
}

//export milib_delay_options_default
func milib_delay_options_default() (out C.milib_delay_options) {
	return out
}

//export milib_test_delay
func milib_test_delay(instance *C.milib_instance, proxyName *C.char, options *C.milib_delay_options) (out C.milib_delay_result) {
	defer func() {
		if r := recover(); r != nil {
			failDelayResult(&out, fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || proxyName == nil {
		failDelayResult(&out, fmt.Errorf("invalid argument"))
		return out
	}
	c, err := core.Get(uint64(C.milib_instance_get_handle(instance)))
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

//export milib_test_link
func milib_test_link(instance *C.milib_instance, uri *C.char, options *C.milib_delay_options) (out C.milib_delay_result) {
	defer func() {
		if r := recover(); r != nil {
			failDelayResult(&out, fmt.Errorf("panic: %v", r))
		}
	}()
	if instance == nil || uri == nil {
		failDelayResult(&out, fmt.Errorf("invalid argument"))
		return out
	}
	c, err := core.Get(uint64(C.milib_instance_get_handle(instance)))
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
