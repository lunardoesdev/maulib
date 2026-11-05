package main

import "core:c"
import "core:fmt"
import "core:os"
import "core:strconv"
import "core:unicode/utf8"
import rl "vendor:raylib"

PAGE_SIZE :: 12
WINDOW_W :: 2000
WINDOW_H :: 1200

g_font: rl.Font
g_symbol_font: rl.Font
g_has_symbol_font: bool
g_truncate_buffer: [512]u8

glyph_exists :: proc(font: rl.Font, r: rune) -> bool {
	if font.glyphs == nil || font.glyphCount <= 0 {
		return false
	}
	index := int(rl.GetGlyphIndex(font, r))
	if index < 0 || index >= int(font.glyphCount) {
		return false
	}
	return font.glyphs[index].value == r
}

font_index_for :: proc(r: rune) -> int {
	if glyph_exists(g_font, r) {
		return 0
	}
	if g_has_symbol_font && glyph_exists(g_symbol_font, r) {
		return 1
	}
	return 0
}

font_at :: proc(index: int) -> rl.Font {
	if index == 1 && g_has_symbol_font {
		return g_symbol_font
	}
	return g_font
}

draw_text :: proc(text: cstring, x, y, size: c.int, color: rl.Color) {
	s := string(text)
	position := f32(x)
	segment: [512]u8
	segment_len := 0
	current_font := -1

	flush := proc(segment: ^[512]u8, segment_len: ^int, font_index: int, position: ^f32,
		y: c.int, size: c.int, color: rl.Color) {
		if segment_len^ == 0 {
			return
		}
		segment[segment_len^] = 0
		font := font_at(font_index)
		value := rl.MeasureTextEx(font, cstring(&segment[0]), f32(size), 0)
		rl.DrawTextEx(font, cstring(&segment[0]), rl.Vector2{position^, f32(y)}, f32(size), 0, color)
		position^ += value.x
		segment_len^ = 0
	}

	for i := 0; i < len(s); {
		r, n := utf8.decode_rune(s[i:])
		if n <= 0 {
			r = '?'
			n = 1
		}
		font_index := font_index_for(r)
		if font_index != current_font && segment_len > 0 {
			flush(&segment, &segment_len, current_font, &position, y, size, color)
		}
		current_font = font_index
		if segment_len + n >= len(segment) {
			flush(&segment, &segment_len, current_font, &position, y, size, color)
		}
		for j := 0; j < n; j += 1 {
			segment[segment_len] = s[i + j]
			segment_len += 1
		}
		i += n
	}
	if segment_len > 0 {
		flush(&segment, &segment_len, current_font, &position, y, size, color)
	}
}

measure_text :: proc(text: cstring, size: c.int) -> c.int {
	s := string(text)
	width := f32(0)
	segment: [512]u8
	segment_len := 0
	current_font := -1

	flush := proc(segment: ^[512]u8, segment_len: ^int, font_index: int, width: ^f32,
		size: c.int) {
		if segment_len^ == 0 {
			return
		}
		segment[segment_len^] = 0
		value := rl.MeasureTextEx(font_at(font_index), cstring(&segment[0]), f32(size), 0)
		width^ += value.x
		segment_len^ = 0
	}

	for i := 0; i < len(s); {
		r, n := utf8.decode_rune(s[i:])
		if n <= 0 {
			r = '?'
			n = 1
		}
		font_index := font_index_for(r)
		if font_index != current_font && segment_len > 0 {
			flush(&segment, &segment_len, current_font, &width, size)
		}
		current_font = font_index
		if segment_len + n >= len(segment) {
			flush(&segment, &segment_len, current_font, &width, size)
		}
		for j := 0; j < n; j += 1 {
			segment[segment_len] = s[i + j]
			segment_len += 1
		}
		i += n
	}
	if segment_len > 0 {
		flush(&segment, &segment_len, current_font, &width, size)
	}
	return c.int(width)
}

truncate_text :: proc(text: cstring, width: c.int, size: c.int) -> cstring {
	s := string(text)
	n := min(len(s), len(g_truncate_buffer) - 1)
	copy(g_truncate_buffer[:n], s[:n])
	g_truncate_buffer[n] = 0
	if n == 0 || measure_text(cstring(&g_truncate_buffer[0]), size) <= width {
		return cstring(&g_truncate_buffer[0])
	}
	low := 0
	high := n
	for low < high {
		mid := (low + high + 1) / 2
		g_truncate_buffer[mid] = 0
		if measure_text(cstring(&g_truncate_buffer[0]), size) <= width {
			low = mid
		} else {
			high = mid - 1
		}
	}
	g_truncate_buffer[low] = 0
	return cstring(&g_truncate_buffer[0])
}

Input_Field :: enum {
	NONE,
	SUB_URL,
	SUB_PRIORITY,
	PROXY_LINK,
}

App :: struct {
	db: ^Mewnet_Subs,

	groups: Mewnet_Subs_Group_List,
	selected_group: i64,

	subs: Mewnet_Subscription_List,
	selected_sub: i64,
	sub_scroll: f32,

	proxies: Mewnet_Subs_Proxy_Summary_List,
	total_proxies: i64,
	page: int,
	selected_proxy: i64,
	detail: Mewnet_Subs_Proxy,
	detail_loaded: bool,

	status: [512]u8,

	sub_url: [MEWNET_URL_MAX]u8,
	sub_url_len: int,
	sub_priority: [32]u8,
	sub_priority_len: int,
	proxy_link: [MEWNET_LINK_MAX]u8,
	proxy_link_len: int,

	active_input: Input_Field,
}

set_status :: proc(app: ^App, text: string) {
	n := min(len(text), len(app.status) - 1)
	copy(app.status[:n], text[:n])
	app.status[n] = 0
}

set_error :: proc(app: ^App, prefix: string, err: Mewnet_Error) {
	local := err
	set_status(app, fmt.tprintf("%s: %s", prefix, string(err_string(&local))))
}

clear_input :: proc(app: ^App, field: Input_Field) {
	switch field {
	case .SUB_URL:
		app.sub_url_len = 0
		app.sub_url[0] = 0
	case .SUB_PRIORITY:
		app.sub_priority_len = 0
		app.sub_priority[0] = 0
	case .PROXY_LINK:
		app.proxy_link_len = 0
		app.proxy_link[0] = 0
	case .NONE:
	}
}

append_input :: proc(app: ^App, r: rune) {
	if r < 32 || r > 126 {
		return
	}
	switch app.active_input {
	case .SUB_URL:
		if app.sub_url_len + 1 < len(app.sub_url) {
			app.sub_url[app.sub_url_len] = u8(r)
			app.sub_url_len += 1
			app.sub_url[app.sub_url_len] = 0
		}
	case .SUB_PRIORITY:
		if app.sub_priority_len + 1 < len(app.sub_priority) {
			app.sub_priority[app.sub_priority_len] = u8(r)
			app.sub_priority_len += 1
			app.sub_priority[app.sub_priority_len] = 0
		}
	case .PROXY_LINK:
		if app.proxy_link_len + 1 < len(app.proxy_link) {
			app.proxy_link[app.proxy_link_len] = u8(r)
			app.proxy_link_len += 1
			app.proxy_link[app.proxy_link_len] = 0
		}
	case .NONE:
	}
}

backspace_input :: proc(app: ^App) {
	switch app.active_input {
	case .SUB_URL:
		if app.sub_url_len > 0 {
			app.sub_url_len -= 1
			app.sub_url[app.sub_url_len] = 0
		}
	case .SUB_PRIORITY:
		if app.sub_priority_len > 0 {
			app.sub_priority_len -= 1
			app.sub_priority[app.sub_priority_len] = 0
		}
	case .PROXY_LINK:
		if app.proxy_link_len > 0 {
			app.proxy_link_len -= 1
			app.proxy_link[app.proxy_link_len] = 0
		}
	case .NONE:
	}
}

reload_proxies :: proc(app: ^App) {
	if app.proxies.items != nil {
		mewnet_subs_unload_proxy_summaries(app.proxies)
		app.proxies = {}
	}
	app.detail_loaded = false
	app.selected_proxy = 0
	app.total_proxies = 0
	if app.selected_group == 0 {
		return
	}

	count: i64
	err := mewnet_subs_count_proxies(app.db, app.selected_group, &count)
	if err.code != .NONE {
		set_error(app, "count proxies", err)
		return
	}
	app.total_proxies = count
	max_page := int((count + PAGE_SIZE - 1) / PAGE_SIZE) - 1
	if max_page < 0 {
		max_page = 0
	}
	if app.page > max_page {
		app.page = max_page
	}
	if app.page < 0 {
		app.page = 0
	}

	offset := uint(app.page) * PAGE_SIZE
	app.proxies = mewnet_subs_load_proxy_summaries(app.db, app.selected_group, PAGE_SIZE, offset)
	if app.proxies.error.code != .NONE {
		set_error(app, "load proxies", app.proxies.error)
	}
}

reload_groups :: proc(app: ^App) {
	if app.groups.items != nil {
		mewnet_subs_unload_groups(app.groups)
		app.groups = {}
	}
	app.groups = mewnet_subs_load_groups(app.db)
	if app.groups.error.code != .NONE {
		set_error(app, "load groups", app.groups.error)
		return
	}
	found := false
	for i in 0 ..< app.groups.count {
		if app.groups.items[i].id == app.selected_group {
			found = true
			break
		}
	}
	if !found {
		app.selected_group = app.groups.count > 0 ? app.groups.items[0].id : 0
		app.page = 0
	}
	reload_subscriptions(app)
}

reload_subscriptions :: proc(app: ^App) {
	if app.subs.items != nil {
		mewnet_subs_unload_subscriptions(app.subs)
		app.subs = {}
	}
	app.selected_sub = 0
	if app.selected_group == 0 {
		reload_proxies(app)
		return
	}
	app.subs = mewnet_subs_load_subscriptions_in_group(app.db, app.selected_group)
	if app.subs.error.code != .NONE {
		set_error(app, "load subscriptions", app.subs.error)
		return
	}
	if app.subs.count > 0 {
		app.selected_sub = app.subs.items[0].id
	}
	reload_proxies(app)
}

add_subscription :: proc(app: ^App) {
	url := cstr(app.sub_url[:])
	if len(url) == 0 {
		set_status(app, "subscription URL is required")
		return
	}
	priority := 0
	if app.sub_priority_len > 0 {
		if v, ok := strconv.parse_int(cstr(app.sub_priority[:]), 10); ok {
			priority = v
		}
	}
	err := mewnet_subs_add_subscription_in_group(app.db, app.selected_group,
		cstring(&app.sub_url[0]), c.int(priority), nil)
	if err.code != .NONE {
		set_error(app, "add subscription", err)
		return
	}
	clear_input(app, .SUB_URL)
	set_status(app, "subscription added")
	reload_subscriptions(app)
}

add_proxy :: proc(app: ^App) {
	if app.selected_sub == 0 {
		set_status(app, "select a subscription first")
		return
	}
	link := cstr(app.proxy_link[:])
	if len(link) == 0 {
		set_status(app, "proxy link is required")
		return
	}
	err := mewnet_subs_add_proxy(app.db, app.selected_group, cstring(&app.proxy_link[0]))
	if err.code != .NONE {
		set_error(app, "add proxy", err)
		return
	}
	clear_input(app, .PROXY_LINK)
	set_status(app, "proxy added")
	reload_proxies(app)
}

remove_subscription :: proc(app: ^App) {
	if app.selected_sub == 0 {
		set_status(app, "no subscription selected")
		return
	}
	err := mewnet_subs_remove_subscription(app.db, app.selected_sub)
	if err.code != .NONE {
		set_error(app, "remove subscription", err)
		return
	}
	set_status(app, "subscription removed")
	app.page = 0
	reload_subscriptions(app)
}

remove_proxy :: proc(app: ^App) {
	if app.selected_proxy == 0 {
		set_status(app, "no proxy selected")
		return
	}
	err := mewnet_subs_remove_proxy(app.db, app.selected_proxy)
	if err.code != .NONE {
		set_error(app, "remove proxy", err)
		return
	}
	set_status(app, "proxy removed")
	reload_proxies(app)
}

load_detail :: proc(app: ^App, proxy_id: i64) {
	app.selected_proxy = proxy_id
	app.detail_loaded = false
	if proxy_id == 0 {
		return
	}
	err := mewnet_subs_get_proxy(app.db, proxy_id, &app.detail)
	if err.code != .NONE {
		set_error(app, "load proxy", err)
		app.selected_proxy = 0
		return
	}
	app.detail_loaded = true
}

button :: proc(rect: rl.Rectangle, label: cstring, enabled := true) -> bool {
	mouse := rl.GetMousePosition()
	hover := enabled && rl.CheckCollisionPointRec(mouse, rect)
	color := !enabled ? rl.LIGHTGRAY : (hover ? rl.LIGHTGRAY : rl.GRAY)
	rl.DrawRectangleRec(rect, color)
	rl.DrawRectangleLinesEx(rect, 1, enabled ? rl.DARKGRAY : rl.GRAY)
	draw_text(label, c.int(rect.x + 14), c.int(rect.y + 14), 32,
		enabled ? rl.BLACK : rl.GRAY)
	return hover && rl.IsMouseButtonPressed(.LEFT)
}

input_box :: proc(rect: rl.Rectangle, label: cstring, value: []u8, active: bool) {
	draw_text(label, c.int(rect.x), c.int(rect.y - 30), 28, rl.DARKGRAY)
	rl.DrawRectangleRec(rect, rl.WHITE)
	rl.DrawRectangleLinesEx(rect, active ? 2 : 1, active ? rl.BLUE : rl.LIGHTGRAY)
	text := value[0] == 0 ? cstring("") : cstring(&value[0])
	draw_text(text, c.int(rect.x + 12), c.int(rect.y + 14), 32, rl.BLACK)
}

draw_wrapped :: proc(text: string, x, y, width: c.int, size: c.int, color: rl.Color) -> c.int {
	line: [512]u8
	line_len := 0
	yy := y
	max_chars := min(len(text), 2400)
	for i in 0 ..< max_chars {
		ch := text[i]
		if ch == '\n' {
			line[line_len] = 0
			draw_text(cstring(&line[0]), x, yy, size, color)
			yy += size + 2
			line_len = 0
			continue
		}
		if line_len + 1 >= len(line) {
			line[line_len] = 0
			draw_text(cstring(&line[0]), x, yy, size, color)
			yy += size + 2
			line_len = 0
		}
		line[line_len] = ch
		line_len += 1
		line[line_len] = 0
		if measure_text(cstring(&line[0]), size) > width {
			line[line_len - 1] = 0
			draw_text(cstring(&line[0]), x, yy, size, color)
			yy += size + 2
			line[0] = ch
			line[1] = 0
			line_len = 1
		}
	}
	if line_len > 0 {
		line[line_len] = 0
		draw_text(cstring(&line[0]), x, yy, size, color)
		yy += size + 2
	}
	return yy
}

main :: proc() {
	db_path := "data/subs.db"
	screenshot_path := ""
	use_default_font := false
	argi := 1
	for argi < len(os.args) {
		if os.args[argi] == "--db" && argi + 1 < len(os.args) {
			db_path = os.args[argi + 1]
			argi += 2
		} else if os.args[argi] == "--screenshot" && argi + 1 < len(os.args) {
			screenshot_path = os.args[argi + 1]
			argi += 2
		} else if os.args[argi] == "--default-font" {
			use_default_font = true
			argi += 1
		} else {
			argi += 1
		}
	}

	config := mewnet_subs_config_default()
	config.db_path = cstring(raw_data(db_path))
	opened := mewnet_subs_open(&config)
	if opened.db == nil {
		fmt.eprintln("open database:", string(err_string(&opened.error)))
		os.exit(1)
	}

	app: App
	app.db = opened.db
	defer {
		if app.proxies.items != nil {
			mewnet_subs_unload_proxy_summaries(app.proxies)
		}
		if app.subs.items != nil {
			mewnet_subs_unload_subscriptions(app.subs)
		}
		if app.groups.items != nil {
			mewnet_subs_unload_groups(app.groups)
		}
		mewnet_subs_close(app.db)
	}

	reload_groups(&app)

	rl.SetConfigFlags({.WINDOW_RESIZABLE})
	rl.InitWindow(WINDOW_W, WINDOW_H, "mewnet subscription manager")
	defer rl.CloseWindow()
	rl.SetTargetFPS(60)

	g_font = rl.GetFontDefault()
	font_loaded := false
	symbol_loaded := false
	font_path := "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"
	if !use_default_font && os.exists(font_path) {
		codepoints := make([dynamic]rune, 0, 2048)
		defer delete(codepoints)
		ranges := [][2]rune{
			{32, 126},
			{0xA0, 0x24F},
			{0x300, 0x36F},
			{0x370, 0x3FF},
			{0x400, 0x52F},
			{0x590, 0x5FF},
			{0x600, 0x6FF},
			{0x750, 0x77F},
		}
		for rng in ranges {
			for r in rng[0] ..= rng[1] {
				append(&codepoints, r)
			}
		}
		loaded := rl.LoadFontEx(cstring(raw_data(font_path)), 52,
			raw_data(codepoints), c.int(len(codepoints)))
		if loaded.texture.id != 0 {
			g_font = loaded
			font_loaded = true
		}
	}

	symbol_path := "/usr/share/fonts/gdouros-symbola/Symbola.ttf"
	if !use_default_font && os.exists(symbol_path) {
		codepoints := make([dynamic]rune, 0, 8192)
		defer delete(codepoints)
		ranges := [][2]rune{
			{0x2000, 0x2BFF},
			{0x1F000, 0x1FAFF},
			{0x1F1E6, 0x1F1FF},
			{0x2600, 0x27BF},
			{0x1F300, 0x1F5FF},
			{0x1F600, 0x1F64F},
			{0x1F680, 0x1F6FF},
			{0x1F900, 0x1F9FF},
		}
		for rng in ranges {
			for r in rng[0] ..= rng[1] {
				append(&codepoints, r)
			}
		}
		loaded := rl.LoadFontEx(cstring(raw_data(symbol_path)), 52,
			raw_data(codepoints), c.int(len(codepoints)))
		if loaded.texture.id != 0 {
			g_symbol_font = loaded
			g_has_symbol_font = true
			symbol_loaded = true
		}
	}
	defer {
		if font_loaded {
			rl.UnloadFont(g_font)
		}
		if symbol_loaded {
			rl.UnloadFont(g_symbol_font)
		}
	}

	group_list_rect := rl.Rectangle{x = 20, y = 124, width = 700, height = 180}
	sub_list_rect := rl.Rectangle{x = 20, y = 364, width = 700, height = 470}
	sub_url_rect := rl.Rectangle{x = 20, y = 900, width = 700, height = 56}
	sub_prio_rect := rl.Rectangle{x = 20, y = 1000, width = 180, height = 56}
	sub_add_rect := rl.Rectangle{x = 220, y = 1000, width = 200, height = 56}
	sub_del_rect := rl.Rectangle{x = 440, y = 1000, width = 220, height = 56}

	proxy_list_rect := rl.Rectangle{x = 760, y = 160, width = 1220, height = 700}
	detail_rect := rl.Rectangle{x = 760, y = 900, width = 1220, height = 120}
	proxy_link_rect := rl.Rectangle{x = 760, y = 1060, width = 700, height = 56}
	proxy_add_rect := rl.Rectangle{x = 1480, y = 1060, width = 200, height = 56}
	proxy_del_rect := rl.Rectangle{x = 1700, y = 1060, width = 280, height = 56}

	screenshot_frames := 0
	for !rl.WindowShouldClose() {
		mouse := rl.GetMousePosition()

		if rl.CheckCollisionPointRec(mouse, sub_url_rect) && rl.IsMouseButtonPressed(.LEFT) {
			app.active_input = .SUB_URL
		} else if rl.CheckCollisionPointRec(mouse, sub_prio_rect) && rl.IsMouseButtonPressed(.LEFT) {
			app.active_input = .SUB_PRIORITY
		} else if rl.CheckCollisionPointRec(mouse, proxy_link_rect) && rl.IsMouseButtonPressed(.LEFT) {
			app.active_input = .PROXY_LINK
		}

		if app.active_input != .NONE {
			for {
				r := rl.GetCharPressed()
				if r == 0 {
					break
				}
				append_input(&app, r)
			}
			if rl.IsKeyPressed(.BACKSPACE) {
				backspace_input(&app)
			}
			if rl.IsKeyPressed(.ESCAPE) {
				app.active_input = .NONE
			}
			if rl.IsKeyPressed(.ENTER) {
				switch app.active_input {
				case .SUB_URL, .SUB_PRIORITY:
					add_subscription(&app)
				case .PROXY_LINK:
					add_proxy(&app)
				case .NONE:
				}
			}
		}

		if rl.CheckCollisionPointRec(mouse, group_list_rect) && rl.IsMouseButtonPressed(.LEFT) {
			y := mouse.y - group_list_rect.y
			index := int(y / 52)
			if index >= 0 && index < int(app.groups.count) {
				id := app.groups.items[index].id
				if id != app.selected_group {
					app.selected_group = id
					app.page = 0
					app.active_input = .NONE
					reload_subscriptions(&app)
				}
			}
		}

		if rl.CheckCollisionPointRec(mouse, sub_list_rect) {
			wheel := rl.GetMouseWheelMove()
			if wheel != 0 {
				app.sub_scroll -= wheel * 52
				max_scroll := max(f32(app.subs.count) * 52 - sub_list_rect.height, 0)
				app.sub_scroll = clamp(app.sub_scroll, f32(0), max_scroll)
			}
			if rl.IsMouseButtonPressed(.LEFT) {
				y := mouse.y - sub_list_rect.y + app.sub_scroll
				index := int(y / 52)
				if index >= 0 && index < int(app.subs.count) {
					id := app.subs.items[index].id
					if id != app.selected_sub {
						app.selected_sub = id
						app.active_input = .NONE
					}
				}
			}
		}

		if rl.CheckCollisionPointRec(mouse, proxy_list_rect) && rl.IsMouseButtonPressed(.LEFT) {
			y := mouse.y - proxy_list_rect.y
			index := int(y / 52)
			if index >= 0 && index < int(app.proxies.count) {
				load_detail(&app, app.proxies.items[index].id)
				app.active_input = .NONE
			}
		}

		rl.BeginDrawing()
		rl.ClearBackground(rl.RAYWHITE)

		if button(sub_add_rect, "Add sub") {
			app.active_input = .NONE
			add_subscription(&app)
		}
		if button(sub_del_rect, "Remove", app.selected_sub != 0) {
			app.active_input = .NONE
			remove_subscription(&app)
		}
		if button(proxy_add_rect, "Add proxy", app.selected_group != 0) {
			app.active_input = .NONE
			add_proxy(&app)
		}
		if button(proxy_del_rect, "Remove proxy", app.selected_proxy != 0) {
			app.active_input = .NONE
			remove_proxy(&app)
		}

		max_page := int((app.total_proxies + PAGE_SIZE - 1) / PAGE_SIZE) - 1
		if max_page < 0 {
			max_page = 0
		}
		prev_rect := rl.Rectangle{x = 1660, y = 70, width = 140, height = 50}
		next_rect := rl.Rectangle{x = 1820, y = 70, width = 150, height = 50}
		if button(prev_rect, "< Prev", app.page > 0) {
			app.page -= 1
			app.active_input = .NONE
			reload_proxies(&app)
		}
		if button(next_rect, "Next >", app.page < max_page) {
			app.page += 1
			app.active_input = .NONE
			reload_proxies(&app)
		}

		draw_text("mewnet subscription manager", 20, 20, 48, rl.DARKGRAY)
		draw_text("Groups", 20, 84, 36, rl.DARKGRAY)

		rl.DrawRectangleRec(group_list_rect, rl.WHITE)
		rl.DrawRectangleLinesEx(group_list_rect, 1, rl.LIGHTGRAY)
		gy := group_list_rect.y
		for i in 0 ..< app.groups.count {
			item := &app.groups.items[i]
			if item.id == app.selected_group {
				rl.DrawRectangle(c.int(group_list_rect.x), c.int(gy),
					c.int(group_list_rect.width), 52, rl.BEIGE)
			}
			group_label := fmt.ctprintf("%s  (priority %d)", cstr(item.name[:]), item.priority)
			draw_text(truncate_text(group_label, c.int(group_list_rect.width - 32), 32),
				c.int(group_list_rect.x + 16), c.int(gy + 14), 32, rl.BLACK)
			gy += 52
		}

		draw_text("Subscriptions in group", 20, 324, 36, rl.DARKGRAY)
		rl.DrawRectangleRec(sub_list_rect, rl.WHITE)
		rl.DrawRectangleLinesEx(sub_list_rect, 1, rl.LIGHTGRAY)
		rl.BeginScissorMode(c.int(sub_list_rect.x), c.int(sub_list_rect.y),
			c.int(sub_list_rect.width), c.int(sub_list_rect.height))
		row_y := sub_list_rect.y - app.sub_scroll
		for i in 0 ..< app.subs.count {
			item := &app.subs.items[i]
			if row_y + 52 >= sub_list_rect.y && row_y <= sub_list_rect.y + sub_list_rect.height {
				if item.id == app.selected_sub {
					rl.DrawRectangle(c.int(sub_list_rect.x), c.int(row_y),
						c.int(sub_list_rect.width), 30, rl.BEIGE)
				}
				name := cstr(item.name[:])
				if len(name) == 0 {
					name = cstr(item.url[:])
				}
				label := fmt.ctprintf("[%d] %s", item.priority, name)
				draw_text(truncate_text(label, c.int(sub_list_rect.width - 32), 32),
				c.int(sub_list_rect.x + 16), c.int(row_y + 14), 32, rl.BLACK)
			}
			row_y += 52
		}
		rl.EndScissorMode()

		input_box(sub_url_rect, "Subscription URL", app.sub_url[:], app.active_input == .SUB_URL)
		input_box(sub_prio_rect, "Priority", app.sub_priority[:], app.active_input == .SUB_PRIORITY)

		group_name := "No group"
		for i in 0 ..< app.groups.count {
			if app.groups.items[i].id == app.selected_group {
				group_name = cstr(app.groups.items[i].name[:])
				break
			}
		}
		draw_text(truncate_text(cstring(raw_data(group_name)), 800, 44), 760, 20, 44, rl.DARKGRAY)
		page_label := fmt.ctprintf("proxies: %d  page %d/%d",
			app.total_proxies, app.page + 1, max_page + 1)
		draw_text(page_label, 760, 84, 30, rl.DARKGRAY)

		rl.DrawRectangleRec(proxy_list_rect, rl.WHITE)
		rl.DrawRectangleLinesEx(proxy_list_rect, 1, rl.LIGHTGRAY)
		rl.BeginScissorMode(c.int(proxy_list_rect.x), c.int(proxy_list_rect.y),
			c.int(proxy_list_rect.width), c.int(proxy_list_rect.height))
		py := proxy_list_rect.y
		for i in 0 ..< app.proxies.count {
			item := &app.proxies.items[i]
			selected := item.id == app.selected_proxy
			if selected {
				rl.DrawRectangle(c.int(proxy_list_rect.x), c.int(py),
					c.int(proxy_list_rect.width), 52, rl.BEIGE)
			}
			name := cstr(item.name[:])
			if len(name) == 0 {
				name = cstr(item.protocol[:])
			}
			draw_text(truncate_text(cstring(raw_data(name)), 390, 30),
				c.int(proxy_list_rect.x + 16), c.int(py + 14), 30, rl.BLACK)
			draw_text(truncate_text(cstring(raw_data(cstr(item.protocol[:]))), 90, 30),
				c.int(proxy_list_rect.x + 430), c.int(py + 14), 30, rl.DARKGRAY)
			addr := fmt.ctprintf("%s:%d", cstr(item.server[:]), item.port)
			draw_text(truncate_text(addr, 320, 30),
				c.int(proxy_list_rect.x + 540), c.int(py + 14), 30, rl.DARKGRAY)
			rating := fmt.ctprintf("r=%d", item.rating)
			draw_text(truncate_text(rating, 60, 30), c.int(proxy_list_rect.x + 880), c.int(py + 14), 30,
				item.rating > 0 ? rl.DARKGREEN : (item.rating < 0 ? rl.MAROON : rl.DARKGRAY))
			alive := fmt.ctprintf("%s%s", item.alive ? "alive" : "dead", item.manual ? " manual" : "")
			draw_text(truncate_text(alive, 100, 30), c.int(proxy_list_rect.x + 960), c.int(py + 14), 30,
				item.alive ? rl.DARKGREEN : rl.MAROON)
			if item.last_delay_ms > 0 {
				delay := fmt.ctprintf("%ums", item.last_delay_ms)
				draw_text(truncate_text(delay, 100, 30),
				c.int(proxy_list_rect.x + 1060), c.int(py + 14), 30, rl.DARKGRAY)
			}
			py += 52
		}
		rl.EndScissorMode()

		rl.DrawRectangleRec(detail_rect, rl.WHITE)
		rl.DrawRectangleLinesEx(detail_rect, 1, rl.LIGHTGRAY)
		if app.detail_loaded {
			head := fmt.ctprintf("id=%d %s  %s:%d  rating=%d  %s%s",
				app.detail.id, cstr(app.detail.name[:]), cstr(app.detail.server[:]),
				app.detail.port, app.detail.rating,
				app.detail.alive ? "alive" : "dead",
				app.detail.manual ? " manual" : "")
			draw_text(head, c.int(detail_rect.x + 16), c.int(detail_rect.y + 14), 30, rl.BLACK)
			draw_wrapped(cstr(app.detail.link[:]), c.int(detail_rect.x + 10),
				c.int(detail_rect.y + 54), c.int(detail_rect.width - 32), 26, rl.DARKGRAY)
		} else {
			draw_text("Select a proxy to see its link", c.int(detail_rect.x + 16),
				c.int(detail_rect.y + 50), 30, rl.GRAY)
		}

		input_box(proxy_link_rect, "Manual proxy link", app.proxy_link[:], app.active_input == .PROXY_LINK)

		status_text := app.status[0] == 0 ? cstring("") : cstring(&app.status[0])
		rl.DrawRectangle(0, WINDOW_H - 60, WINDOW_W, 60, rl.LIGHTGRAY)
		draw_text(status_text, 20, WINDOW_H - 46, 30, rl.DARKGRAY)

		rl.EndDrawing()
		if len(screenshot_path) > 0 {
			screenshot_frames += 1
			if screenshot_frames >= 2 {
				rl.TakeScreenshot(cstring(raw_data(screenshot_path)))
				break
			}
		}
	}
}
