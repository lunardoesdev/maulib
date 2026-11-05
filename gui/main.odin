package main

import "core:c"
import "core:fmt"
import "core:os"
import "core:strconv"
import rl "vendor:raylib"

PAGE_SIZE :: 16
WINDOW_W :: 1200
WINDOW_H :: 760

g_font: rl.Font
g_codepoints: [512]rune

draw_text :: proc(text: cstring, x, y, size: c.int, color: rl.Color) {
	rl.DrawTextEx(g_font, text, rl.Vector2{f32(x), f32(y)}, f32(size), 0, color)
}

measure_text :: proc(text: cstring, size: c.int) -> c.int {
	value := rl.MeasureTextEx(g_font, text, f32(size), 0)
	return c.int(value.x)
}

Input_Field :: enum {
	NONE,
	SUB_URL,
	SUB_PRIORITY,
	PROXY_LINK,
}

App :: struct {
	db: ^Milib_Subs,

	subs: Milib_Subscription_List,
	selected_sub: i64,
	sub_scroll: f32,

	proxies: Milib_Subs_Proxy_Summary_List,
	total_proxies: i64,
	page: int,
	selected_proxy: i64,
	detail: Milib_Subs_Proxy,
	detail_loaded: bool,

	status: [512]u8,

	sub_url: [MILIB_URL_MAX]u8,
	sub_url_len: int,
	sub_priority: [32]u8,
	sub_priority_len: int,
	proxy_link: [MILIB_LINK_MAX]u8,
	proxy_link_len: int,

	active_input: Input_Field,
}

set_status :: proc(app: ^App, text: string) {
	n := min(len(text), len(app.status) - 1)
	copy(app.status[:n], text[:n])
	app.status[n] = 0
}

set_error :: proc(app: ^App, prefix: string, err: Milib_Error) {
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
		milib_subs_unload_proxy_summaries(app.proxies)
		app.proxies = {}
	}
	app.detail_loaded = false
	app.selected_proxy = 0
	app.total_proxies = 0
	if app.selected_sub == 0 {
		return
	}

	count: i64
	err := milib_subs_count_proxies(app.db, app.selected_sub, &count)
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
	app.proxies = milib_subs_load_proxy_summaries(app.db, app.selected_sub, PAGE_SIZE, offset)
	if app.proxies.error.code != .NONE {
		set_error(app, "load proxies", app.proxies.error)
	}
}

reload_subscriptions :: proc(app: ^App) {
	if app.subs.items != nil {
		milib_subs_unload_subscriptions(app.subs)
		app.subs = {}
	}
	app.subs = milib_subs_load_subscriptions(app.db)
	if app.subs.error.code != .NONE {
		set_error(app, "load subscriptions", app.subs.error)
		return
	}

	found := false
	for i in 0 ..< app.subs.count {
		if app.subs.items[i].id == app.selected_sub {
			found = true
			break
		}
	}
	if !found {
		app.selected_sub = app.subs.count > 0 ? app.subs.items[0].id : 0
		app.page = 0
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
	err := milib_subs_add_subscription(app.db, cstring(&app.sub_url[0]), c.int(priority), nil)
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
	err := milib_subs_add_proxy(app.db, app.selected_sub, cstring(&app.proxy_link[0]))
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
	err := milib_subs_remove_subscription(app.db, app.selected_sub)
	if err.code != .NONE {
		set_error(app, "remove subscription", err)
		return
	}
	set_status(app, "subscription removed")
	app.selected_sub = 0
	app.page = 0
	reload_subscriptions(app)
}

remove_proxy :: proc(app: ^App) {
	if app.selected_proxy == 0 {
		set_status(app, "no proxy selected")
		return
	}
	err := milib_subs_remove_proxy(app.db, app.selected_proxy)
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
	err := milib_subs_get_proxy(app.db, proxy_id, &app.detail)
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
	color := !enabled ? rl.DARKGRAY : (hover ? rl.LIGHTGRAY : rl.GRAY)
	rl.DrawRectangleRec(rect, color)
	rl.DrawRectangleLinesEx(rect, 1, rl.DARKGRAY)
	draw_text(label, c.int(rect.x + 8), c.int(rect.y + 6), 16,
		enabled ? rl.BLACK : rl.DARKGRAY)
	return hover && rl.IsMouseButtonPressed(.LEFT)
}

input_box :: proc(rect: rl.Rectangle, label: cstring, value: []u8, active: bool) {
	draw_text(label, c.int(rect.x), c.int(rect.y - 16), 14, rl.DARKGRAY)
	rl.DrawRectangleRec(rect, rl.WHITE)
	rl.DrawRectangleLinesEx(rect, active ? 2 : 1, active ? rl.BLUE : rl.LIGHTGRAY)
	text := value[0] == 0 ? cstring("") : cstring(&value[0])
	draw_text(text, c.int(rect.x + 6), c.int(rect.y + 7), 16, rl.BLACK)
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
	if len(os.args) >= 3 && os.args[1] == "--db" {
		db_path = os.args[2]
	}

	config := milib_subs_config_default()
	config.db_path = cstring(raw_data(db_path))
	opened := milib_subs_open(&config)
	if opened.db == nil {
		fmt.eprintln("open database:", string(err_string(&opened.error)))
		os.exit(1)
	}

	app: App
	app.db = opened.db
	defer {
		if app.proxies.items != nil {
			milib_subs_unload_proxy_summaries(app.proxies)
		}
		if app.subs.items != nil {
			milib_subs_unload_subscriptions(app.subs)
		}
		milib_subs_close(app.db)
	}

	reload_subscriptions(&app)

	rl.SetConfigFlags({.WINDOW_RESIZABLE})
	rl.InitWindow(WINDOW_W, WINDOW_H, "milib subscription manager")
	defer rl.CloseWindow()
	rl.SetTargetFPS(60)

	g_font = rl.GetFontDefault()
	font_path := "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"
	if os.exists(font_path) {
		index := 0
		for r in 32 ..= 126 {
			g_codepoints[index] = rune(r)
			index += 1
		}
		for r in 0x400 ..= 0x4FF {
			g_codepoints[index] = rune(r)
			index += 1
		}
		loaded := rl.LoadFontEx(cstring(raw_data(font_path)), 20, &g_codepoints[0], c.int(index))
		if loaded.texture.id != 0 {
			g_font = loaded
			defer rl.UnloadFont(g_font)
		}
	}

	list_rect := rl.Rectangle{x = 10, y = 56, width = 340, height = 420}
	sub_url_rect := rl.Rectangle{x = 10, y = 516, width = 340, height = 34}
	sub_prio_rect := rl.Rectangle{x = 10, y = 576, width = 100, height = 34}
	sub_add_rect := rl.Rectangle{x = 120, y = 576, width = 110, height = 34}
	sub_del_rect := rl.Rectangle{x = 240, y = 576, width = 110, height = 34}

	proxy_list_rect := rl.Rectangle{x = 365, y = 90, width = 820, height = 430}
	proxy_link_rect := rl.Rectangle{x = 365, y = 612, width = 570, height = 34}
	proxy_add_rect := rl.Rectangle{x = 945, y = 612, width = 110, height = 34}
	proxy_del_rect := rl.Rectangle{x = 1065, y = 612, width = 120, height = 34}
	detail_rect := rl.Rectangle{x = 365, y = 540, width = 820, height = 60}

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

		if rl.CheckCollisionPointRec(mouse, list_rect) {
			wheel := rl.GetMouseWheelMove()
			if wheel != 0 {
				app.sub_scroll -= wheel * 30
				max_scroll := max(f32(app.subs.count) * 26 - list_rect.height, 0)
				app.sub_scroll = clamp(app.sub_scroll, 0, max_scroll)
			}
			if rl.IsMouseButtonPressed(.LEFT) {
				y := mouse.y - list_rect.y + app.sub_scroll
				index := int(y / 26)
				if index >= 0 && index < int(app.subs.count) {
					id := app.subs.items[index].id
					if id != app.selected_sub {
						app.selected_sub = id
						app.page = 0
						app.active_input = .NONE
						reload_proxies(&app)
					}
				}
			}
		}

		if rl.CheckCollisionPointRec(mouse, proxy_list_rect) && rl.IsMouseButtonPressed(.LEFT) {
			y := mouse.y - proxy_list_rect.y
			index := int(y / 26)
			if index >= 0 && index < int(app.proxies.count) {
				load_detail(&app, app.proxies.items[index].id)
				app.active_input = .NONE
			}
		}

		if button(sub_add_rect, "Add sub") {
			app.active_input = .NONE
			add_subscription(&app)
		}
		if button(sub_del_rect, "Remove", app.selected_sub != 0) {
			app.active_input = .NONE
			remove_subscription(&app)
		}
		if button(proxy_add_rect, "Add proxy", app.selected_sub != 0) {
			app.active_input = .NONE
			add_proxy(&app)
		}
		if button(proxy_del_rect, "Remove proxy", app.selected_proxy != 0) {
			app.active_input = .NONE
			remove_proxy(&app)
		}

		prev_rect := rl.Rectangle{x = 960, y = 48, width = 90, height = 30}
		next_rect := rl.Rectangle{x = 1070, y = 48, width = 90, height = 30}
		if button(prev_rect, "< Prev", app.page > 0) {
			app.page -= 1
			app.active_input = .NONE
			reload_proxies(&app)
		}
		max_page := int((app.total_proxies + PAGE_SIZE - 1) / PAGE_SIZE) - 1
		if max_page < 0 {
			max_page = 0
		}
		if button(next_rect, "Next >", app.page < max_page) {
			app.page += 1
			app.active_input = .NONE
			reload_proxies(&app)
		}

		rl.BeginDrawing()
		rl.ClearBackground(rl.RAYWHITE)

		draw_text("milib subscription manager", 10, 14, 24, rl.DARKGRAY)
		draw_text("Subscriptions", 10, 36, 16, rl.DARKGRAY)

		rl.DrawRectangleRec(list_rect, rl.WHITE)
		rl.DrawRectangleLinesEx(list_rect, 1, rl.LIGHTGRAY)
		rl.BeginScissorMode(c.int(list_rect.x), c.int(list_rect.y),
			c.int(list_rect.width), c.int(list_rect.height))
		row_y := list_rect.y - app.sub_scroll
		for i in 0 ..< app.subs.count {
			item := &app.subs.items[i]
			if row_y + 26 >= list_rect.y && row_y <= list_rect.y + list_rect.height {
				selected := item.id == app.selected_sub
				if selected {
					rl.DrawRectangle(c.int(list_rect.x), c.int(row_y), c.int(list_rect.width), 26, rl.BEIGE)
				}
				name := cstr(item.name[:])
				if len(name) == 0 {
					name = cstr(item.url[:])
				}
				label := fmt.ctprintf("[%d] %s", item.priority, name)
				draw_text(label, c.int(list_rect.x + 6), c.int(row_y + 5), 14, rl.BLACK)
			}
			row_y += 26
		}
		rl.EndScissorMode()

		input_box(sub_url_rect, "Subscription URL", app.sub_url[:], app.active_input == .SUB_URL)
		input_box(sub_prio_rect, "Priority", app.sub_priority[:], app.active_input == .SUB_PRIORITY)

		sub_name := "No subscription selected"
		if app.selected_sub != 0 {
			for i in 0 ..< app.subs.count {
				if app.subs.items[i].id == app.selected_sub {
					n := cstr(app.subs.items[i].name[:])
					if len(n) == 0 {
						n = cstr(app.subs.items[i].url[:])
					}
					sub_name = n
					break
				}
			}
		}
		draw_text(cstring(raw_data(sub_name)), 365, 14, 20, rl.DARKGRAY)
		page_label := fmt.ctprintf("proxies: %d  page %d/%d",
			app.total_proxies, app.page + 1, max_page + 1)
		draw_text(page_label, 365, 52, 16, rl.DARKGRAY)

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
					c.int(proxy_list_rect.width), 26, rl.BEIGE)
			}
			name := cstr(item.name[:])
			if len(name) == 0 {
				name = cstr(item.protocol[:])
			}
			draw_text(cstring(raw_data(name)), c.int(proxy_list_rect.x + 6), c.int(py + 5), 14, rl.BLACK)
			draw_text(cstring(raw_data(cstr(item.protocol[:]))), c.int(proxy_list_rect.x + 330), c.int(py + 5), 14, rl.DARKGRAY)
			addr := fmt.ctprintf("%s:%d", cstr(item.server[:]), item.port)
			draw_text(addr, c.int(proxy_list_rect.x + 400), c.int(py + 5), 14, rl.DARKGRAY)
			rating := fmt.ctprintf("r=%d", item.rating)
			draw_text(rating, c.int(proxy_list_rect.x + 650), c.int(py + 5), 14,
				item.rating > 0 ? rl.DARKGREEN : (item.rating < 0 ? rl.MAROON : rl.DARKGRAY))
			alive := fmt.ctprintf("%s%s", item.alive ? "alive" : "dead", item.manual ? " manual" : "")
			draw_text(alive, c.int(proxy_list_rect.x + 710), c.int(py + 5), 14,
				item.alive ? rl.DARKGREEN : rl.MAROON)
			if item.last_delay_ms > 0 {
				delay := fmt.ctprintf("%ums", item.last_delay_ms)
				draw_text(delay, c.int(proxy_list_rect.x + 800), c.int(py + 5), 14, rl.DARKGRAY)
			}
			py += 26
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
			draw_text(head, c.int(detail_rect.x + 8), c.int(detail_rect.y + 6), 14, rl.BLACK)
			draw_wrapped(cstr(app.detail.link[:]), c.int(detail_rect.x + 8),
				c.int(detail_rect.y + 26), c.int(detail_rect.width - 16), 13, rl.DARKGRAY)
		} else {
			draw_text("Select a proxy to see its link", c.int(detail_rect.x + 8),
				c.int(detail_rect.y + 20), 14, rl.GRAY)
		}

		input_box(proxy_link_rect, "Manual proxy link", app.proxy_link[:], app.active_input == .PROXY_LINK)

		status_text := app.status[0] == 0 ? cstring("") : cstring(&app.status[0])
		rl.DrawRectangle(0, WINDOW_H - 28, WINDOW_W, 28, rl.LIGHTGRAY)
		draw_text(status_text, 10, WINDOW_H - 22, 16, rl.DARKGRAY)

		rl.EndDrawing()
	}
}
