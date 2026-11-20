package main

/*
#include "bridge.h"
*/
import "C"

import (
	"context"
	"fmt"
	"io"
	"net/http"
	"time"
	"unsafe"
)

const (
	defaultHTTPTimeoutMS = 30000
	maxHTTPTimeoutMS     = 300000
	defaultRedirects     = 5
	maxRedirects         = 20
	httpReadBuffer       = 32 * 1024
)

//export mewnet_http_stream
func mewnet_http_stream(url *C.char, options *C.mewnet_http_options, cb C.mewnet_http_cb, user unsafe.Pointer) (out C.mewnet_http_result) {
	defer func() {
		if r := recover(); r != nil {
			out = C.mewnet_http_result{}
			out.error.code = C.MEWNET_ERROR_FAILED
			copyCString(out.error.message[:], fmt.Sprintf("panic: %v", r))
		}
	}()

	if url == nil {
		out.error.code = C.MEWNET_ERROR_INVALID_ARGUMENT
		copyCString(out.error.message[:], "url is required")
		return out
	}

	timeout := defaultHTTPTimeoutMS
	userAgent := "mewnet/0.3"
	etag := ""
	lastModified := ""
	redirects := defaultRedirects
	if options != nil {
		if options.timeout_ms > 0 {
			timeout = int(options.timeout_ms)
		}
		if timeout > maxHTTPTimeoutMS {
			timeout = maxHTTPTimeoutMS
		}
		if options.user_agent != nil {
			userAgent = goString(options.user_agent)
		}
		etag = goString(options.etag)
		lastModified = goString(options.last_modified)
		if options.max_redirects > 0 {
			redirects = int(options.max_redirects)
		}
		if redirects > maxRedirects {
			redirects = maxRedirects
		}
	}

	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(timeout)*time.Millisecond)
	defer cancel()

	client := &http.Client{
		CheckRedirect: func(_ *http.Request, via []*http.Request) error {
			if len(via) >= redirects {
				return fmt.Errorf("stopped after %d redirects", redirects)
			}
			return nil
		},
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, goString(url), nil)
	if err != nil {
		out.error.code = C.MEWNET_ERROR_INVALID_ARGUMENT
		copyCString(out.error.message[:], err.Error())
		return out
	}
	if userAgent != "" {
		req.Header.Set("User-Agent", userAgent)
	}
	if etag != "" {
		req.Header.Set("If-None-Match", etag)
	}
	if lastModified != "" {
		req.Header.Set("If-Modified-Since", lastModified)
	}

	resp, err := client.Do(req)
	if err != nil {
		out.error.code = cErrorCode(err)
		copyCString(out.error.message[:], err.Error())
		logError(err)
		return out
	}
	defer resp.Body.Close()

	out.status = C.int(resp.StatusCode)
	copyCString(out.etag[:], resp.Header.Get("ETag"))
	copyCString(out.last_modified[:], resp.Header.Get("Last-Modified"))
	copyCString(out.subscription_userinfo[:], resp.Header.Get("subscription-userinfo"))
	copyCString(out.profile_title[:], resp.Header.Get("profile-title"))
	copyCString(out.profile_update_interval[:], resp.Header.Get("profile-update-interval"))

	if resp.StatusCode == http.StatusNotModified {
		out.ok = true
		out.error.code = C.MEWNET_ERROR_NONE
		return out
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		err = fmt.Errorf("http status %d", resp.StatusCode)
		out.error.code = C.MEWNET_ERROR_FAILED
		copyCString(out.error.message[:], err.Error())
		logError(err)
		return out
	}

	buffer := make([]byte, httpReadBuffer)
	for {
		n, readErr := resp.Body.Read(buffer)
		if n > 0 {
			out.bytes += C.ulonglong(n)
			if cb != nil {
				if C.mewnet_call_http_cb(cb, unsafe.Pointer(&buffer[0]), C.size_t(n), user) != 0 {
					out.error.code = C.MEWNET_ERROR_CANCELLED
					copyCString(out.error.message[:], "http callback aborted")
					return out
				}
			}
		}
		if readErr == io.EOF {
			break
		}
		if readErr != nil {
			out.error.code = cErrorCode(readErr)
			copyCString(out.error.message[:], readErr.Error())
			logError(readErr)
			return out
		}
	}

	out.ok = true
	out.error.code = C.MEWNET_ERROR_NONE
	return out
}
