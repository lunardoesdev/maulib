package main

/*
#include "bridge.h"
*/
import "C"

import (
	"context"
	"fmt"
	"sync"
	"unsafe"

	"mewnet/internal/core"
)

//export mewnet_test_links_batch
func mewnet_test_links_batch(instance *C.mewnet_instance, links **C.char, count C.size_t, options *C.mewnet_delay_options, concurrency C.size_t) (out C.mewnet_batch_test_result) {
	return testLinksBatch(instance, links, count, options, concurrency, false)
}

//export mewnet_test_links_batch_first
func mewnet_test_links_batch_first(instance *C.mewnet_instance, links **C.char, count C.size_t, options *C.mewnet_delay_options, concurrency C.size_t) (out C.mewnet_batch_test_result) {
	return testLinksBatch(instance, links, count, options, concurrency, true)
}

func testLinksBatch(instance *C.mewnet_instance, links **C.char, count C.size_t, options *C.mewnet_delay_options, concurrency C.size_t, stopOnSuccess bool) (out C.mewnet_batch_test_result) {
	defer func() {
		if r := recover(); r != nil {
			out = C.mewnet_batch_test_result{}
			setCError(&out.error, fmt.Errorf("panic: %v", r))
		}
	}()

	if instance == nil {
		setCError(&out.error, fmt.Errorf("invalid instance"))
		return out
	}
	n := int(count)
	if n <= 0 || links == nil {
		setCError(&out.error, fmt.Errorf("links are required"))
		return out
	}
	c, err := core.Get(uint64(C.mewnet_instance_get_handle(instance)))
	if err != nil {
		setCError(&out.error, err)
		return out
	}

	items, err := allocItems[C.mewnet_link_test_result](n)
	if err != nil {
		setCError(&out.error, err)
		return out
	}

	handles := unsafe.Slice(links, n)
	linksGo := make([]string, n)
	for i, handle := range handles {
		linksGo[i] = goString(handle)
	}
	opts := parseDelayOptions(options)
	workers := int(concurrency)
	if workers <= 0 {
		workers = 1
	}
	if workers > n {
		workers = n
	}
	if workers > 256 {
		workers = 256
	}

	ctx := context.Background()
	cancel := func() {}
	if stopOnSuccess {
		ctx, cancel = context.WithCancel(ctx)
	}
	defer cancel()

	slice := unsafe.Slice(items, n)
	jobs := make(chan int)
	var wg sync.WaitGroup
	var foundMu sync.Mutex
	found := false

	for w := 0; w < workers; w++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for index := range jobs {
				if stopOnSuccess {
					select {
					case <-ctx.Done():
						failLinkTestResult(&slice[index], context.Canceled)
						continue
					default:
					}
				}
				testOneLinkContext(c, ctx, linksGo[index], opts, &slice[index])
				if stopOnSuccess && bool(slice[index].ok) {
					foundMu.Lock()
					if !found {
						found = true
						cancel()
					}
					foundMu.Unlock()
				}
			}
		}()
	}
	for i := 0; i < n; i++ {
		jobs <- i
	}
	close(jobs)
	wg.Wait()

	out.ok = true
	out.error.code = C.MEWNET_ERROR_NONE
	out.items = items
	out.count = C.size_t(n)
	return out
}

func testOneLinkContext(c *core.Core, ctx context.Context, link string, opts core.DelayOptions, dst *C.mewnet_link_test_result) {
	defer func() {
		if r := recover(); r != nil {
			failLinkTestResult(dst, fmt.Errorf("panic: %v", r))
		}
	}()
	result, err := c.TestLinkContext(ctx, link, opts)
	if err != nil {
		failLinkTestResult(dst, err)
		return
	}
	fillLinkTestResult(dst, *result)
}

//export mewnet_unload_batch_test
func mewnet_unload_batch_test(result C.mewnet_batch_test_result) {
	if result.items != nil {
		C.free(unsafe.Pointer(result.items))
	}
}
