package main

import "core:c"

MEWNET_NAME_MAX :: 512
MEWNET_TYPE_MAX :: 32
MEWNET_HOST_MAX :: 256
MEWNET_URL_MAX :: 2048
MEWNET_LINK_MAX :: 8192
MEWNET_ERROR_MAX :: 512

Mewnet_Error_Code :: enum c.int {
	NONE             = 0,
	FAILED           = 1,
	INVALID_ARGUMENT = 2,
	NOT_FOUND        = 3,
	TIMEOUT          = 4,
	CANCELLED         = 5,
	CLOSED            = 6,
}

Mewnet_Error :: struct {
	code:    Mewnet_Error_Code,
	message: [MEWNET_ERROR_MAX]u8,
}

Mewnet_Subs :: struct {}

Mewnet_Subs_Config :: struct {
	db_path:                    cstring,
	user_agent:                 cstring,
	test_url:                   cstring,
	test_expected:              cstring,
	test_timeout_ms:            c.int,
	request_limit:              c.int,
	request_window_seconds:     c.int,
	default_fetch_interval_sec: c.int,
}

Mewnet_Subs_Open_Result :: struct {
	db:    ^Mewnet_Subs,
	error: Mewnet_Error,
}

Mewnet_Subs_Group :: struct {
	id:         i64,
	name:       [MEWNET_NAME_MAX]u8,
	priority:   c.int,
	enabled:    bool,
	created_at: i64,
}

Mewnet_Subs_Group_List :: struct {
	error: Mewnet_Error,
	items: [^]Mewnet_Subs_Group,
	count: uint,
}

Mewnet_Subscription :: struct {
	id:              i64,
	group_id:        i64,
	url:             [MEWNET_URL_MAX]u8,
	name:            [MEWNET_NAME_MAX]u8,
	priority:        c.int,
	enabled:         bool,
	last_fetch_at:   i64,
	next_fetch_at:   i64,
	last_status:     c.int,
	failure_count:   c.int,
	etag:            [256]u8,
	last_modified:   [256]u8,
	userinfo:        [512]u8,
	last_error:      [MEWNET_ERROR_MAX]u8,
}

Mewnet_Subscription_List :: struct {
	error: Mewnet_Error,
	items: [^]Mewnet_Subscription,
	count: uint,
}

Mewnet_Subs_Proxy_Summary :: struct {
	id:              i64,
	group_id:        i64,
	name:            [MEWNET_NAME_MAX]u8,
	protocol:        [MEWNET_TYPE_MAX]u8,
	server:          [MEWNET_HOST_MAX]u8,
	port:            c.int,
	rating:          c.int,
	alive:           bool,
	manual:          bool,
	last_tested_at:  i64,
	last_delay_ms:   c.uint,
	last_ttfb_ms:    c.uint,
	last_ttlb_ms:    c.uint,
	last_bytes:      u64,
	last_speed_bps:  u64,
	last_error:      [MEWNET_ERROR_MAX]u8,
}

Mewnet_Subs_Proxy_Summary_List :: struct {
	error: Mewnet_Error,
	items: [^]Mewnet_Subs_Proxy_Summary,
	count: uint,
}

Mewnet_Subs_Proxy :: struct {
	id:              i64,
	group_id:        i64,
	link:            [MEWNET_LINK_MAX]u8,
	name:            [MEWNET_NAME_MAX]u8,
	protocol:        [MEWNET_TYPE_MAX]u8,
	server:          [MEWNET_HOST_MAX]u8,
	port:            c.int,
	rating:          c.int,
	alive:           bool,
	manual:          bool,
	last_tested_at:  i64,
	last_delay_ms:   c.uint,
	last_ttfb_ms:    c.uint,
	last_ttlb_ms:    c.uint,
	last_bytes:      u64,
	last_speed_bps:  u64,
	last_error:      [MEWNET_ERROR_MAX]u8,
}

Mewnet_Subs_Stats :: struct {
	subscriptions: i64,
	proxies:       i64,
	alive:         i64,
	dead:          i64,
	tests:         i64,
}

foreign import mewnet_subs "system:mewnet"

@(default_calling_convention="c")
foreign mewnet_subs {
	mewnet_subs_config_default :: proc() -> Mewnet_Subs_Config ---
	mewnet_subs_open :: proc(config: ^Mewnet_Subs_Config) -> Mewnet_Subs_Open_Result ---
	mewnet_subs_close :: proc(db: ^Mewnet_Subs) ---

	mewnet_subs_load_groups :: proc(db: ^Mewnet_Subs) -> Mewnet_Subs_Group_List ---
	mewnet_subs_unload_groups :: proc(list: Mewnet_Subs_Group_List) ---
	mewnet_subs_add_group :: proc(db: ^Mewnet_Subs, name: cstring, priority: c.int) -> Mewnet_Error ---
	mewnet_subs_remove_group :: proc(db: ^Mewnet_Subs, group_id: i64) -> Mewnet_Error ---
	mewnet_subs_set_group_priority :: proc(db: ^Mewnet_Subs, group_id: i64, priority: c.int) -> Mewnet_Error ---
	mewnet_subs_set_subscription_group :: proc(db: ^Mewnet_Subs, subscription_id: i64, group_id: i64) -> Mewnet_Error ---
	mewnet_subs_default_group :: proc(db: ^Mewnet_Subs, out_group_id: ^i64) -> Mewnet_Error ---

	mewnet_subs_add_subscription :: proc(db: ^Mewnet_Subs, url: cstring, priority: c.int, name: cstring) -> Mewnet_Error ---
	mewnet_subs_add_subscription_in_group :: proc(db: ^Mewnet_Subs, group_id: i64, url: cstring, priority: c.int, name: cstring) -> Mewnet_Error ---
	mewnet_subs_remove_subscription :: proc(db: ^Mewnet_Subs, id: i64) -> Mewnet_Error ---
	mewnet_subs_set_priority :: proc(db: ^Mewnet_Subs, id: i64, priority: c.int) -> Mewnet_Error ---
	mewnet_subs_set_enabled :: proc(db: ^Mewnet_Subs, id: i64, enabled: bool) -> Mewnet_Error ---

	mewnet_subs_load_subscriptions :: proc(db: ^Mewnet_Subs) -> Mewnet_Subscription_List ---
	mewnet_subs_load_subscriptions_in_group :: proc(db: ^Mewnet_Subs, group_id: i64) -> Mewnet_Subscription_List ---
	mewnet_subs_unload_subscriptions :: proc(list: Mewnet_Subscription_List) ---

	mewnet_subs_load_proxy_summaries :: proc(db: ^Mewnet_Subs, group_id: i64, limit: uint, offset: uint) -> Mewnet_Subs_Proxy_Summary_List ---
	mewnet_subs_unload_proxy_summaries :: proc(list: Mewnet_Subs_Proxy_Summary_List) ---
	mewnet_subs_get_proxy :: proc(db: ^Mewnet_Subs, proxy_id: i64, out: ^Mewnet_Subs_Proxy) -> Mewnet_Error ---
	mewnet_subs_add_proxy :: proc(db: ^Mewnet_Subs, group_id: i64, link: cstring) -> Mewnet_Error ---
	mewnet_subs_remove_proxy :: proc(db: ^Mewnet_Subs, proxy_id: i64) -> Mewnet_Error ---
	mewnet_subs_count_proxies :: proc(db: ^Mewnet_Subs, group_id: i64, out_count: ^i64) -> Mewnet_Error ---

	mewnet_subs_get_stats :: proc(db: ^Mewnet_Subs) -> Mewnet_Subs_Stats ---
}

cstr :: proc(buf: []u8) -> string {
	if len(buf) == 0 || buf[0] == 0 {
		return ""
	}
	return string(cstring(&buf[0]))
}

err_string :: proc(err: ^Mewnet_Error) -> cstring {
	if err == nil || err.message[0] == 0 {
		return "unknown error"
	}
	return cstring(&err.message[0])
}
