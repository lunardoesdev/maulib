package main

import "core:c"

MILIB_NAME_MAX :: 512
MILIB_TYPE_MAX :: 32
MILIB_HOST_MAX :: 256
MILIB_URL_MAX :: 2048
MILIB_LINK_MAX :: 8192
MILIB_ERROR_MAX :: 512

Milib_Error_Code :: enum c.int {
	NONE             = 0,
	FAILED           = 1,
	INVALID_ARGUMENT = 2,
	NOT_FOUND        = 3,
	TIMEOUT          = 4,
	CANCELLED         = 5,
	CLOSED            = 6,
}

Milib_Error :: struct {
	code:    Milib_Error_Code,
	message: [MILIB_ERROR_MAX]u8,
}

Milib_Subs :: struct {}

Milib_Subs_Config :: struct {
	db_path:                    cstring,
	user_agent:                 cstring,
	test_url:                   cstring,
	test_expected:              cstring,
	test_timeout_ms:            c.int,
	request_limit:              c.int,
	request_window_seconds:     c.int,
	default_fetch_interval_sec: c.int,
}

Milib_Subs_Open_Result :: struct {
	db:    ^Milib_Subs,
	error: Milib_Error,
}

Milib_Subscription :: struct {
	id:              i64,
	url:             [MILIB_URL_MAX]u8,
	name:            [MILIB_NAME_MAX]u8,
	priority:        c.int,
	enabled:         bool,
	last_fetch_at:   i64,
	next_fetch_at:   i64,
	last_status:     c.int,
	failure_count:   c.int,
	etag:            [256]u8,
	last_modified:   [256]u8,
	userinfo:        [512]u8,
	last_error:      [MILIB_ERROR_MAX]u8,
}

Milib_Subscription_List :: struct {
	error: Milib_Error,
	items: [^]Milib_Subscription,
	count: uint,
}

Milib_Subs_Proxy_Summary :: struct {
	id:              i64,
	subscription_id: i64,
	name:            [MILIB_NAME_MAX]u8,
	protocol:        [MILIB_TYPE_MAX]u8,
	server:          [MILIB_HOST_MAX]u8,
	port:            c.int,
	rating:          c.int,
	alive:           bool,
	manual:          bool,
	last_tested_at:  i64,
	last_delay_ms:   c.uint,
	last_error:      [MILIB_ERROR_MAX]u8,
}

Milib_Subs_Proxy_Summary_List :: struct {
	error: Milib_Error,
	items: [^]Milib_Subs_Proxy_Summary,
	count: uint,
}

Milib_Subs_Proxy :: struct {
	id:              i64,
	subscription_id: i64,
	link:            [MILIB_LINK_MAX]u8,
	name:            [MILIB_NAME_MAX]u8,
	protocol:        [MILIB_TYPE_MAX]u8,
	server:          [MILIB_HOST_MAX]u8,
	port:            c.int,
	rating:          c.int,
	alive:           bool,
	manual:          bool,
	last_tested_at:  i64,
	last_delay_ms:   c.uint,
	last_error:      [MILIB_ERROR_MAX]u8,
}

Milib_Subs_Stats :: struct {
	subscriptions: i64,
	proxies:       i64,
	alive:         i64,
	dead:          i64,
	tests:         i64,
}

foreign import milib_subs "system:milib_subs"

@(default_calling_convention="c")
foreign milib_subs {
	milib_subs_config_default :: proc() -> Milib_Subs_Config ---
	milib_subs_open :: proc(config: ^Milib_Subs_Config) -> Milib_Subs_Open_Result ---
	milib_subs_close :: proc(db: ^Milib_Subs) ---

	milib_subs_add_subscription :: proc(db: ^Milib_Subs, url: cstring, priority: c.int, name: cstring) -> Milib_Error ---
	milib_subs_remove_subscription :: proc(db: ^Milib_Subs, id: i64) -> Milib_Error ---
	milib_subs_set_priority :: proc(db: ^Milib_Subs, id: i64, priority: c.int) -> Milib_Error ---
	milib_subs_set_enabled :: proc(db: ^Milib_Subs, id: i64, enabled: bool) -> Milib_Error ---

	milib_subs_load_subscriptions :: proc(db: ^Milib_Subs) -> Milib_Subscription_List ---
	milib_subs_unload_subscriptions :: proc(list: Milib_Subscription_List) ---

	milib_subs_load_proxy_summaries :: proc(db: ^Milib_Subs, subscription_id: i64, limit: uint, offset: uint) -> Milib_Subs_Proxy_Summary_List ---
	milib_subs_unload_proxy_summaries :: proc(list: Milib_Subs_Proxy_Summary_List) ---
	milib_subs_get_proxy :: proc(db: ^Milib_Subs, proxy_id: i64, out: ^Milib_Subs_Proxy) -> Milib_Error ---
	milib_subs_add_proxy :: proc(db: ^Milib_Subs, subscription_id: i64, link: cstring) -> Milib_Error ---
	milib_subs_remove_proxy :: proc(db: ^Milib_Subs, proxy_id: i64) -> Milib_Error ---
	milib_subs_count_proxies :: proc(db: ^Milib_Subs, subscription_id: i64, out_count: ^i64) -> Milib_Error ---

	milib_subs_get_stats :: proc(db: ^Milib_Subs) -> Milib_Subs_Stats ---
}

cstr :: proc(buf: []u8) -> string {
	if len(buf) == 0 || buf[0] == 0 {
		return ""
	}
	return string(cstring(&buf[0]))
}

err_string :: proc(err: ^Milib_Error) -> cstring {
	if err == nil || err.message[0] == 0 {
		return "unknown error"
	}
	return cstring(&err.message[0])
}
