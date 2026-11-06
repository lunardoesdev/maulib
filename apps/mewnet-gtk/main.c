/*
 * mewnet-gtk - GTK4 front-end for the mewnet discovery agent.
 *
 * All reliability logic (health checks, failover, pool growth, subscription
 * scheduling) lives in libmewnet_subs. This file only renders state and sends
 * user actions to the agent.
 */

#include "mewnet_subs.h"

#include <gtk/gtk.h>
#include <glib-unix.h>

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define APP_ID "org.mewnet.gtk"
#define LOG_MAX_LINES 2000
#define LOG_KEEP_LINES 1500
#define PAGE_SIZE 50

typedef struct App App;

typedef struct Options {
    char* db_path;
    int proxy_port;
    size_t batch;
    size_t concurrency;
    int timeout_ms;
    int fetch_interval_sec;
    int slow_interval_sec;
    int pool_target;
    int health_interval_sec;
    int health_timeout_ms;
    char* health_url;
    char* health_expected;
    char* test_url;
    char* test_expected;
    char* screenshot_path;
    GPtrArray* urls;
} Options;

struct App {
    GtkWidget* window;
    GtkWidget* local_proxy_label;
    GtkWidget* state_label;
    GtkWidget* current_name_label;
    GtkWidget* current_detail_label;
    GtkWidget* counts_label;
    GtkWidget* switch_button;
    GtkWidget* subs_list;
    GtkWidget* subs_empty_label;
    GtkWidget* subs_entry;
    GtkWidget* remove_button;
    GtkWidget* prev_button;
    GtkWidget* next_button;
    GtkWidget* page_label;
    GtkWidget* log_view;
    GtkTextBuffer* log_buffer;

    Options options;
    mewnet_subs* db;
    mewnet_subs* ui_db;
    mewnet_instance* milib;
    mewnet_agent* agent;
    int64_t group_id;
    int actual_port;

    gint closing;

    GArray* sub_ids;
    int64_t selected_sub_id;
    size_t ui_offset;
    int64_t ui_total;
    gint reload_pending;

    guint screenshot_source;
};

/* ------------------------------------------------------------------ */
/* logging                                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    App* app;
    char* text;
} LogMessage;

static gboolean append_log_idle(gpointer data) {
    LogMessage* message = (LogMessage*)data;
    App* app = message->app;
    if (!g_atomic_int_get(&app->closing) && app->log_buffer != NULL) {
        GtkTextIter end;
        gtk_text_buffer_get_end_iter(app->log_buffer, &end);
        gtk_text_buffer_insert(app->log_buffer, &end, message->text, -1);

        int lines = gtk_text_buffer_get_line_count(app->log_buffer);
        if (lines > LOG_MAX_LINES) {
            GtkTextIter start;
            GtkTextIter cut;
            gtk_text_buffer_get_start_iter(app->log_buffer, &start);
            gtk_text_buffer_get_iter_at_line(app->log_buffer, &cut, lines - LOG_KEEP_LINES);
            gtk_text_buffer_delete(app->log_buffer, &start, &cut);
        }
        if (app->log_view != NULL) {
            gtk_text_buffer_get_end_iter(app->log_buffer, &end);
            GtkTextMark* mark = gtk_text_buffer_create_mark(app->log_buffer, NULL, &end, FALSE);
            gtk_text_view_scroll_mark_onscreen(GTK_TEXT_VIEW(app->log_view), mark);
            gtk_text_buffer_delete_mark(app->log_buffer, mark);
        }
    }
    g_free(message->text);
    g_free(message);
    return G_SOURCE_REMOVE;
}

static void schedule_reload_impl(App* app);

static void log_line(App* app, const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    char stamp[32];
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);

    char* line = g_strdup_printf("%s  %s\n", stamp, text);
    g_print("%s", line);
    fflush(stdout);

    LogMessage* message = g_new0(LogMessage, 1);
    message->app = app;
    message->text = line;
    g_idle_add(append_log_idle, message);
}

static void agent_log(const char* message, void* user) {
    App* app = (App*)user;
    if (g_atomic_int_get(&app->closing)) {
        return;
    }
    log_line(app, "%s", message);
    if (strncmp(message, "FETCH ", 6) == 0) {
        schedule_reload_impl(app);
    }
}

/* ------------------------------------------------------------------ */
/* state rendering                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    App* app;
    mewnet_agent_snapshot snapshot;
} StateMessage;

static void update_counts(App* app, const mewnet_agent_snapshot* snapshot) {
    char* counts = g_strdup_printf("subscriptions: %lld · proxies: %lld · alive: %lld · untested: %lld",
                                   (long long)snapshot->subscriptions,
                                   (long long)snapshot->proxies,
                                   (long long)snapshot->alive,
                                   (long long)snapshot->untested);
    gtk_label_set_text(GTK_LABEL(app->counts_label), counts);
    g_free(counts);

    const char* state;
    if (snapshot->subscriptions == 0) {
        state = "Mode: waiting for subscriptions";
    } else if (snapshot->alive > 0) {
        state = "Mode: normal discovery (working proxy available)";
    } else {
        state = "Mode: fast discovery (no working proxy)";
    }
    gtk_label_set_text(GTK_LABEL(app->state_label), state);
}

static gboolean apply_state_idle(gpointer data) {
    StateMessage* message = (StateMessage*)data;
    App* app = message->app;
    if (!g_atomic_int_get(&app->closing)) {
        const mewnet_agent_snapshot* snapshot = &message->snapshot;
        if (snapshot->proxy_id != 0) {
            char* name = g_strdup_printf("Currently using: id=%lld  %s",
                                         (long long)snapshot->proxy_id,
                                         snapshot->name[0] != '\0' ? snapshot->name : "(manual)");
            gtk_label_set_text(GTK_LABEL(app->current_name_label), name);
            g_free(name);

            GString* detail = g_string_new(NULL);
            if (snapshot->protocol[0] != '\0') {
                g_string_append(detail, snapshot->protocol);
            }
            if (snapshot->server[0] != '\0') {
                if (detail->len > 0) g_string_append(detail, " · ");
                g_string_append_printf(detail, "%s:%d", snapshot->server, snapshot->port);
            }
            if (snapshot->delay_ms > 0) {
                if (detail->len > 0) g_string_append(detail, " · ");
                g_string_append_printf(detail, "%u ms", snapshot->delay_ms);
            }
            gtk_label_set_text(GTK_LABEL(app->current_detail_label), detail->str);
            g_string_free(detail, TRUE);
        } else {
            gtk_label_set_text(GTK_LABEL(app->current_name_label), "Currently using: none");
            gtk_label_set_text(GTK_LABEL(app->current_detail_label),
                               "the first working proxy will be selected automatically");
        }
        update_counts(app, snapshot);
    }
    g_free(message);
    return G_SOURCE_REMOVE;
}

static void agent_state(const mewnet_agent_snapshot* snapshot, void* user) {
    App* app = (App*)user;
    if (g_atomic_int_get(&app->closing)) {
        return;
    }
    StateMessage* message = g_new0(StateMessage, 1);
    message->app = app;
    message->snapshot = *snapshot;
    g_idle_add(apply_state_idle, message);
}

/* ------------------------------------------------------------------ */
/* subscriptions                                                       */
/* ------------------------------------------------------------------ */

static void reload_subscriptions(App* app);

static gboolean reload_idle(gpointer data) {
    App* app = (App*)data;
    g_atomic_int_set(&app->reload_pending, 0);
    if (!g_atomic_int_get(&app->closing)) {
        reload_subscriptions(app);
    }
    return G_SOURCE_REMOVE;
}

static void schedule_reload_impl(App* app) {
    if (g_atomic_int_compare_and_exchange(&app->reload_pending, 0, 1)) {
        g_idle_add(reload_idle, app);
    }
}

static void update_page_controls(App* app) {
    size_t first = app->ui_total == 0 ? 0 : app->ui_offset + 1;
    size_t last = app->ui_offset + PAGE_SIZE;
    if (last > (size_t)app->ui_total) last = (size_t)app->ui_total;
    char* text = g_strdup_printf("%zu–%zu of %lld", first, last, (long long)app->ui_total);
    gtk_label_set_text(GTK_LABEL(app->page_label), text);
    g_free(text);
    gtk_widget_set_sensitive(app->prev_button, app->ui_offset > 0);
    gtk_widget_set_sensitive(app->next_button,
                             app->ui_offset + PAGE_SIZE < (size_t)app->ui_total);
}

static void reload_subscriptions(App* app) {
    mewnet_subs_count_subscriptions(app->ui_db, app->group_id, &app->ui_total);
    if (app->ui_offset >= (size_t)app->ui_total && app->ui_total > 0) {
        app->ui_offset = ((size_t)app->ui_total - 1) / PAGE_SIZE * PAGE_SIZE;
    }
    mewnet_subscription_list list = mewnet_subs_load_subscriptions_page(
        app->ui_db, app->group_id, PAGE_SIZE, app->ui_offset);
    if (list.error.code != MEWNET_ERROR_NONE) {
        return;
    }

    int64_t selected = app->selected_sub_id;
    int selected_index = -1;
    int index = 0;
    gtk_list_box_remove_all(GTK_LIST_BOX(app->subs_list));
    g_array_set_size(app->sub_ids, 0);

    for (size_t i = 0; i < list.count; i++) {
        mewnet_subscription* sub = &list.items[i];
        g_array_append_val(app->sub_ids, sub->id);

        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(box, 12);
        gtk_widget_set_margin_end(box, 12);
        gtk_widget_set_margin_top(box, 9);
        gtk_widget_set_margin_bottom(box, 9);

        GtkWidget* url = gtk_label_new(sub->url);
        gtk_label_set_xalign(GTK_LABEL(url), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(url), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(url), 52);
        gtk_widget_set_tooltip_text(url, sub->url);
        gtk_box_append(GTK_BOX(box), url);

        char status[512];
        if (sub->last_fetch_at == 0) {
            snprintf(status, sizeof(status), "not fetched");
        } else if (sub->last_error[0] != '\0') {
            snprintf(status, sizeof(status), "error: %.100s", sub->last_error);
        } else {
            snprintf(status, sizeof(status), "HTTP %d", sub->last_status);
        }
        GtkWidget* meta = gtk_label_new(status);
        gtk_label_set_xalign(GTK_LABEL(meta), 0.0f);
        gtk_label_set_ellipsize(GTK_LABEL(meta), PANGO_ELLIPSIZE_END);
        gtk_widget_add_css_class(meta, "hint");
        gtk_box_append(GTK_BOX(box), meta);

        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
        gtk_list_box_append(GTK_LIST_BOX(app->subs_list), row);
        if (sub->id == selected) {
            selected_index = index;
        }
        index++;
    }

    if (selected_index >= 0) {
        GtkListBoxRow* row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(app->subs_list), selected_index);
        if (row != NULL) {
            gtk_list_box_select_row(GTK_LIST_BOX(app->subs_list), row);
            app->selected_sub_id = selected;
        }
    } else {
        app->selected_sub_id = 0;
        gtk_list_box_unselect_all(GTK_LIST_BOX(app->subs_list));
    }

    gtk_widget_set_visible(app->subs_empty_label, app->ui_total == 0);
    update_page_controls(app);
    mewnet_subs_unload_subscriptions(list);
}

static void on_sub_selection_changed(GtkListBox* box, GtkListBoxRow* row, gpointer user) {
    (void)box;
    App* app = (App*)user;
    if (row == NULL) {
        app->selected_sub_id = 0;
        return;
    }
    int index = gtk_list_box_row_get_index(row);
    if (index >= 0 && index < (int)app->sub_ids->len) {
        app->selected_sub_id = g_array_index(app->sub_ids, int64_t, index);
    }
}

static void on_add_subscription(GtkButton* button, gpointer user) {
    (void)button;
    App* app = (App*)user;
    const char* url = gtk_editable_get_text(GTK_EDITABLE(app->subs_entry));
    if (url == NULL || url[0] == '\0') {
        log_line(app, "ADD: enter a subscription URL");
        return;
    }
    mewnet_error err = mewnet_subs_add_subscription_in_group(
        app->ui_db, app->group_id, url, 0, NULL);
    if (err.code != MEWNET_ERROR_NONE) {
        log_line(app, "ADD FAILED: %s", err.message);
        return;
    }
    gtk_editable_set_text(GTK_EDITABLE(app->subs_entry), "");

    int64_t total = 0;
    mewnet_subs_count_subscriptions(app->ui_db, app->group_id, &total);
    if (total > 0) {
        app->ui_offset = ((size_t)total - 1) / PAGE_SIZE * PAGE_SIZE;
    }
    mewnet_subscription_list list = mewnet_subs_load_subscriptions_page(
        app->ui_db, app->group_id, PAGE_SIZE, app->ui_offset);
    if (list.error.code == MEWNET_ERROR_NONE) {
        for (size_t i = 0; i < list.count; i++) {
            if (strcmp(list.items[i].url, url) == 0) {
                app->selected_sub_id = list.items[i].id;
                if (app->agent != NULL) {
                    mewnet_agent_request_subscription(app->agent, list.items[i].id);
                }
            }
        }
        mewnet_subs_unload_subscriptions(list);
    }
    log_line(app, "ADD ok");
    reload_subscriptions(app);
}

static void on_remove_subscription(GtkButton* button, gpointer user) {
    (void)button;
    App* app = (App*)user;
    if (app->selected_sub_id == 0) {
        log_line(app, "REMOVE: select a subscription first");
        return;
    }
    int64_t id = app->selected_sub_id;
    mewnet_error err = mewnet_subs_remove_subscription(app->ui_db, id);
    if (err.code != MEWNET_ERROR_NONE) {
        log_line(app, "REMOVE FAILED: %s", err.message);
        return;
    }
    log_line(app, "REMOVE ok id=%lld", (long long)id);
    app->selected_sub_id = 0;
    reload_subscriptions(app);
}

static void on_prev_page(GtkButton* button, gpointer user) {
    (void)button;
    App* app = (App*)user;
    if (app->ui_offset >= PAGE_SIZE) {
        app->ui_offset -= PAGE_SIZE;
    } else {
        app->ui_offset = 0;
    }
    reload_subscriptions(app);
}

static void on_next_page(GtkButton* button, gpointer user) {
    (void)button;
    App* app = (App*)user;
    if (app->ui_offset + PAGE_SIZE < (size_t)app->ui_total) {
        app->ui_offset += PAGE_SIZE;
        reload_subscriptions(app);
    }
}

static void on_switch_proxy(GtkButton* button, gpointer user) {
    (void)button;
    App* app = (App*)user;
    if (app->agent != NULL) {
        mewnet_agent_request_switch(app->agent);
    }
}

/* ------------------------------------------------------------------ */
/* UI construction                                                     */
/* ------------------------------------------------------------------ */

static void install_css(void) {
    static const char* css =
        ".card {"
        "  background: alpha(@theme_fg_color, 0.045);"
        "  border: 1px solid alpha(@borders, 0.55);"
        "  border-radius: 14px;"
        "}"
        ".local-proxy { font-family: monospace; font-size: 15px; font-weight: 600; }"
        ".current-name { font-size: 15px; font-weight: 700; }"
        ".state { font-weight: 700; }"
        ".log-view { font-family: monospace; font-size: 12px; }"
        ".hint { opacity: 0.62; font-size: 12px; }"
        ".subs-scroll, .log-scroll {"
        "  border-radius: 12px;"
        "  border: 1px solid alpha(@borders, 0.55);"
        "  background: alpha(@theme_fg_color, 0.025);"
        "}"
        ".subs-list { background: transparent; }"
        ".subs-list > row { border-bottom: 1px solid alpha(@borders, 0.45); }"
        ".subs-list > row:last-child { border-bottom: none; }"
        ".subs-list > row:selected {"
        "  background: alpha(@theme_selected_bg_color, 0.18);"
        "  color: @theme_fg_color;"
        "}"
        "button.suggested-action { border-radius: 10px; font-weight: 600; }"
        "entry { border-radius: 10px; }";
    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    GdkDisplay* display = gdk_display_get_default();
    if (display != NULL) {
        gtk_style_context_add_provider_for_display(display, GTK_STYLE_PROVIDER(provider),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

static void set_margins(GtkWidget* widget, int all) {
    gtk_widget_set_margin_start(widget, all);
    gtk_widget_set_margin_end(widget, all);
    gtk_widget_set_margin_top(widget, all);
    gtk_widget_set_margin_bottom(widget, all);
}

static GtkWidget* section_title(const char* text) {
    GtkWidget* label = gtk_label_new(NULL);
    char* markup = g_strdup_printf("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    return label;
}

static GtkWidget* build_status_panel(App* app) {
    GtkWidget* card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(card, "card");
    gtk_widget_set_margin_start(card, 16);
    gtk_widget_set_margin_end(card, 16);
    gtk_widget_set_margin_top(card, 12);
    gtk_widget_set_margin_bottom(card, 6);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
    set_margins(grid, 16);

    app->local_proxy_label = gtk_label_new("Local proxy:  ...");
    gtk_label_set_xalign(GTK_LABEL(app->local_proxy_label), 0.0f);
    gtk_label_set_selectable(GTK_LABEL(app->local_proxy_label), TRUE);
    gtk_widget_add_css_class(app->local_proxy_label, "local-proxy");
    gtk_widget_set_hexpand(app->local_proxy_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->local_proxy_label, 0, 0, 1, 1);

    app->state_label = gtk_label_new("Mode: starting");
    gtk_label_set_xalign(GTK_LABEL(app->state_label), 0.0f);
    gtk_widget_add_css_class(app->state_label, "state");
    gtk_grid_attach(GTK_GRID(grid), app->state_label, 1, 0, 1, 1);

    app->current_name_label = gtk_label_new("Currently using: none");
    gtk_label_set_xalign(GTK_LABEL(app->current_name_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app->current_name_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(app->current_name_label, "current-name");
    gtk_widget_set_hexpand(app->current_name_label, TRUE);
    gtk_grid_attach(GTK_GRID(grid), app->current_name_label, 0, 1, 1, 1);

    app->switch_button = gtk_button_new_with_label("Change proxy");
    gtk_widget_add_css_class(app->switch_button, "suggested-action");
    g_signal_connect(app->switch_button, "clicked", G_CALLBACK(on_switch_proxy), app);
    gtk_widget_set_halign(app->switch_button, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), app->switch_button, 1, 1, 1, 1);

    app->current_detail_label = gtk_label_new("the first working proxy will be selected automatically");
    gtk_label_set_xalign(GTK_LABEL(app->current_detail_label), 0.0f);
    gtk_label_set_ellipsize(GTK_LABEL(app->current_detail_label), PANGO_ELLIPSIZE_END);
    gtk_widget_add_css_class(app->current_detail_label, "hint");
    gtk_grid_attach(GTK_GRID(grid), app->current_detail_label, 0, 2, 1, 1);

    app->counts_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(app->counts_label), 1.0f);
    gtk_widget_add_css_class(app->counts_label, "hint");
    gtk_grid_attach(GTK_GRID(grid), app->counts_label, 1, 2, 1, 1);

    gtk_box_append(GTK_BOX(card), grid);
    return card;
}

static void build_subscriptions_panel(App* app, GtkWidget* parent) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(box, 12);
    gtk_box_append(GTK_BOX(box), section_title("Subscriptions"));

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(scroller, "subs-scroll");

    app->subs_list = gtk_list_box_new();
    gtk_widget_add_css_class(app->subs_list, "subs-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(app->subs_list), GTK_SELECTION_SINGLE);
    g_signal_connect(app->subs_list, "row-selected", G_CALLBACK(on_sub_selection_changed), app);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app->subs_list);
    gtk_box_append(GTK_BOX(box), scroller);

    app->subs_empty_label = gtk_label_new("No subscriptions yet. Add a URL below.");
    gtk_label_set_xalign(GTK_LABEL(app->subs_empty_label), 0.0f);
    gtk_widget_add_css_class(app->subs_empty_label, "hint");
    gtk_box_append(GTK_BOX(box), app->subs_empty_label);

    GtkWidget* pager = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->prev_button = gtk_button_new_with_label("Prev");
    app->next_button = gtk_button_new_with_label("Next");
    app->page_label = gtk_label_new("");
    gtk_widget_add_css_class(app->page_label, "hint");
    gtk_widget_set_hexpand(app->page_label, TRUE);
    g_signal_connect(app->prev_button, "clicked", G_CALLBACK(on_prev_page), app);
    g_signal_connect(app->next_button, "clicked", G_CALLBACK(on_next_page), app);
    gtk_box_append(GTK_BOX(pager), app->prev_button);
    gtk_box_append(GTK_BOX(pager), app->page_label);
    gtk_box_append(GTK_BOX(pager), app->next_button);
    gtk_box_append(GTK_BOX(box), pager);

    GtkWidget* entry_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    app->subs_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app->subs_entry), "https://example.com/sub");
    gtk_widget_set_hexpand(app->subs_entry, TRUE);
    GtkWidget* add_button = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(add_button, "suggested-action");
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_subscription), app);
    gtk_box_append(GTK_BOX(entry_row), app->subs_entry);
    gtk_box_append(GTK_BOX(entry_row), add_button);
    gtk_box_append(GTK_BOX(box), entry_row);

    app->remove_button = gtk_button_new_with_label("Remove");
    g_signal_connect(app->remove_button, "clicked", G_CALLBACK(on_remove_subscription), app);
    gtk_widget_set_halign(app->remove_button, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), app->remove_button);

    GtkWidget* hint = gtk_label_new("A new subscription is fetched immediately.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0f);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_add_css_class(hint, "hint");
    gtk_box_append(GTK_BOX(box), hint);

    gtk_paned_set_start_child(GTK_PANED(parent), box);
}

static void build_log_panel(App* app, GtkWidget* parent) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    set_margins(box, 12);
    gtk_box_append(GTK_BOX(box), section_title("Discovery log"));

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_add_css_class(scroller, "log-scroll");

    app->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(app->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app->log_view), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(app->log_view), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(app->log_view), GTK_WRAP_WORD_CHAR);
    gtk_widget_add_css_class(app->log_view, "log-view");
    set_margins(app->log_view, 6);
    app->log_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(app->log_view));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), app->log_view);
    gtk_box_append(GTK_BOX(box), scroller);

    gtk_paned_set_end_child(GTK_PANED(parent), box);
}

/* ------------------------------------------------------------------ */
/* services and lifecycle                                              */
/* ------------------------------------------------------------------ */

static void update_local_proxy_label(App* app) {
    char* text = g_strdup_printf("Local proxy:  http://127.0.0.1:%d", app->actual_port);
    gtk_label_set_text(GTK_LABEL(app->local_proxy_label), text);
    g_free(text);
}

static void cleanup(App* app) {
    if (g_atomic_int_get(&app->closing)) {
        return;
    }
    g_atomic_int_set(&app->closing, 1);
    if (app->screenshot_source != 0) {
        g_source_remove(app->screenshot_source);
        app->screenshot_source = 0;
    }
    if (app->agent != NULL) {
        mewnet_agent_stop(app->agent);
        app->agent = NULL;
    }
    if (app->milib != NULL) {
        mewnet_close(app->milib);
        app->milib = NULL;
    }
    if (app->db != NULL) {
        mewnet_subs_close(app->db);
        app->db = NULL;
    }
    if (app->ui_db != NULL) {
        mewnet_subs_close(app->ui_db);
        app->ui_db = NULL;
    }
}

static bool open_services(App* app) {
    char* path_copy = g_strdup(app->options.db_path);
    char* slash = strrchr(path_copy, '/');
    if (slash != NULL) {
        *slash = '\0';
        g_mkdir_with_parents(path_copy, 0755);
    }
    g_free(path_copy);

    mewnet_subs_config config = mewnet_subs_config_default();
    config.db_path = app->options.db_path;
    config.test_url = app->options.test_url;
    config.test_expected = app->options.test_expected;
    config.test_timeout_ms = app->options.timeout_ms;
    config.request_limit = 10;
    config.request_window_seconds = 240;
    config.default_fetch_interval_sec = app->options.fetch_interval_sec;

    mewnet_subs_open_result opened = mewnet_subs_open(&config);
    if (opened.db == NULL) {
        log_line(app, "Could not open database: %s", opened.error.message);
        return false;
    }
    app->db = opened.db;
    mewnet_subs_open_result ui = mewnet_subs_open(&config);
    if (ui.db == NULL) {
        log_line(app, "Could not open UI database: %s", ui.error.message);
        return false;
    }
    app->ui_db = ui.db;

    mewnet_error err = mewnet_subs_default_group(app->ui_db, &app->group_id);
    if (err.code != MEWNET_ERROR_NONE) {
        log_line(app, "Could not create group: %s", err.message);
        return false;
    }
    for (size_t i = 0; i < app->options.urls->len; i++) {
        const char* url = g_ptr_array_index(app->options.urls, i);
        err = mewnet_subs_add_subscription_in_group(app->ui_db, app->group_id, url, 0, NULL);
        if (err.code != MEWNET_ERROR_NONE) {
            log_line(app, "ADD FAILED url=%s: %s", url, err.message);
        }
    }

    mewnet_config milib_config = mewnet_config_default();
    milib_config.mixed_port = app->options.proxy_port;
    milib_config.log_level = "warning";
    mewnet_init_result init = mewnet_init(&milib_config);
    if (init.instance == NULL) {
        log_line(app, "Could not start local proxy: %s", init.error.message);
        return false;
    }
    app->milib = init.instance;
    app->actual_port = mewnet_get_status(app->milib).mixed_port;
    if (app->actual_port <= 0) {
        log_line(app, "Local proxy is not listening");
        return false;
    }
    return true;
}

static gboolean take_screenshot(gpointer data) {
    App* app = (App*)data;
    int width = gtk_widget_get_width(app->window);
    int height = gtk_widget_get_height(app->window);
    if (width <= 0 || height <= 0) {
        return G_SOURCE_REMOVE;
    }
    GtkNative* native = gtk_widget_get_native(app->window);
    if (native == NULL) return G_SOURCE_REMOVE;
    GskRenderer* renderer = gtk_native_get_renderer(native);
    if (renderer == NULL) return G_SOURCE_REMOVE;

    GdkPaintable* paintable = gtk_widget_paintable_new(app->window);
    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(GDK_PAINTABLE(paintable), snapshot, width, height);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);
    graphene_rect_t rect = GRAPHENE_RECT_INIT(0, 0, width, height);
    GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &rect);
    if (texture != NULL) {
        if (gdk_texture_save_to_png(texture, app->options.screenshot_path)) {
            log_line(app, "SCREENSHOT %s", app->options.screenshot_path);
        }
        g_object_unref(texture);
    }
    gsk_render_node_unref(node);
    g_object_unref(paintable);
    if (app->options.screenshot_path != NULL) {
        GApplication* application = g_application_get_default();
        if (application != NULL) g_application_quit(application);
    }
    return G_SOURCE_REMOVE;
}

static gboolean on_close_request(GtkWindow* window, gpointer user) {
    (void)window;
    cleanup((App*)user);
    return FALSE;
}

static gboolean on_unix_signal(gpointer user) {
    (void)user;
    GApplication* application = g_application_get_default();
    if (application != NULL) g_application_quit(application);
    return G_SOURCE_REMOVE;
}

static void on_mihomo_log(mewnet_log_level level, const char* message, void* user) {
    App* app = (App*)user;
    if (g_atomic_int_get(&app->closing)) return;
    const char* tag = level == MEWNET_LOG_ERROR ? "error" :
                      level == MEWNET_LOG_WARNING ? "warn" : "info";
    log_line(app, "[mihomo/%s] %s", tag, message);
}

static void on_activate(GtkApplication* gtk_app, gpointer user) {
    App* app = (App*)user;
    install_css();

    app->window = gtk_application_window_new(gtk_app);
    gtk_window_set_title(GTK_WINDOW(app->window), "mewnet — proxy and discovery");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 1280, 800);
    g_signal_connect(app->window, "close-request", G_CALLBACK(on_close_request), app);

    GtkWidget* root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_window_set_child(GTK_WINDOW(app->window), root);

    GtkWidget* header = gtk_header_bar_new();
    GtkWidget* title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(title), "<b>mewnet</b>  discovery control");
    gtk_header_bar_set_title_widget(GTK_HEADER_BAR(header), title);
    gtk_window_set_titlebar(GTK_WINDOW(app->window), header);

    gtk_box_append(GTK_BOX(root), build_status_panel(app));

    GtkWidget* paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 460);
    gtk_widget_set_vexpand(paned, TRUE);
    gtk_box_append(GTK_BOX(root), paned);
    build_subscriptions_panel(app, paned);
    build_log_panel(app, paned);

    if (!open_services(app)) {
        gtk_widget_set_sensitive(root, FALSE);
        gtk_window_present(GTK_WINDOW(app->window));
        return;
    }

    update_local_proxy_label(app);
    log_line(app, "DB %s", app->options.db_path);
    log_line(app, "PROXY http://127.0.0.1:%d", app->actual_port);
    log_line(app, "batch=%zu concurrency=%zu fetch_interval=%ds slow_interval=%ds",
             app->options.batch, app->options.concurrency,
             app->options.fetch_interval_sec, app->options.slow_interval_sec);

    reload_subscriptions(app);

    mewnet_agent_config agent_config = mewnet_agent_config_default();
    agent_config.db = app->db;
    agent_config.milib = app->milib;
    agent_config.group_id = app->group_id;
    agent_config.proxy_port = app->actual_port;
    agent_config.batch = app->options.batch;
    agent_config.concurrency = app->options.concurrency;
    agent_config.fetch_interval_sec = app->options.fetch_interval_sec;
    agent_config.slow_interval_sec = app->options.slow_interval_sec;
    agent_config.pool_target = app->options.pool_target;
    agent_config.health_interval_sec = app->options.health_interval_sec;
    agent_config.health_timeout_ms = app->options.health_timeout_ms;
    agent_config.health_url = app->options.health_url;
    agent_config.health_expected = app->options.health_expected;
    mewnet_agent_callbacks callbacks = {agent_log, agent_state, app};
    app->agent = mewnet_agent_start(&agent_config, callbacks);
    if (app->agent == NULL) {
        log_line(app, "Could not start discovery agent");
    }

    gtk_window_present(GTK_WINDOW(app->window));
    if (app->options.screenshot_path != NULL) {
        app->screenshot_source = g_timeout_add(1500, take_screenshot, app);
    }
}

/* ------------------------------------------------------------------ */
/* options                                                             */
/* ------------------------------------------------------------------ */

static void options_init(Options* options) {
    memset(options, 0, sizeof(*options));
    options->db_path = g_strdup("data/mewnet-gtk.db");
    options->batch = 70;
    options->concurrency = 70;
    options->timeout_ms = 4000;
    options->fetch_interval_sec = 3600;
    options->slow_interval_sec = 24;
    options->pool_target = 12;
    options->health_interval_sec = 6;
    options->health_timeout_ms = 2500;
    options->health_url = g_strdup("https://lua.org");
    options->health_expected = g_strdup("200");
    options->test_url = g_strdup("https://www.gstatic.com/generate_204");
    options->test_expected = g_strdup("204");
    options->urls = g_ptr_array_new_with_free_func(g_free);
}

static void options_free(Options* options) {
    g_free(options->db_path);
    g_free(options->health_url);
    g_free(options->health_expected);
    g_free(options->test_url);
    g_free(options->test_expected);
    g_free(options->screenshot_path);
    g_ptr_array_unref(options->urls);
}

static bool take_value(int argc, char** argv, int* index, const char** out, GError** error) {
    if (*index + 1 >= argc) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE,
                    "option %s requires a value", argv[*index]);
        return false;
    }
    *out = argv[++(*index)];
    return true;
}

static bool parse_options(int argc, char** argv, Options* options, GError** error) {
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        const char* value = NULL;
        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            g_print("usage: %s [options] [SUBSCRIPTION_URL...]\n"
                    "  --db PATH --proxy-port N --batch N --concurrency N --timeout MS\n"
                    "  --fetch-interval S --slow-interval S --pool-target N\n"
                    "  --health-interval S --health-timeout MS --health-url URL\n"
                    "  --test-url URL --test-expected CODE --screenshot PATH\n",
                    argv[0]);
            exit(0);
        } else if (strcmp(arg, "--db") == 0 && take_value(argc, argv, &i, &value, error)) {
            g_free(options->db_path);
            options->db_path = g_strdup(value);
        } else if (strcmp(arg, "--proxy-port") == 0 && take_value(argc, argv, &i, &value, error)) {
            options->proxy_port = atoi(value);
        } else if (strcmp(arg, "--batch") == 0 && take_value(argc, argv, &i, &value, error)) {
            options->batch = (size_t)atol(value);
        } else if (strcmp(arg, "--concurrency") == 0 && take_value(argc, argv, &i, &value, error)) {
            options->concurrency = (size_t)atol(value);
        } else if (strcmp(arg, "--timeout") == 0 && take_value(argc, argv, &i, &value, error)) {
            options->timeout_ms = atoi(value);
        } else if (strcmp(arg, "--fetch-interval") == 0 && take_value(argc, argv, &i, &value, error)) {
            options->fetch_interval_sec = atoi(value);
        } else if (strcmp(arg, "--slow-interval") == 0 && take_value(argc, argv, &i, &value, error)) {
            options->slow_interval_sec = atoi(value);
        } else if (strcmp(arg, "--pool-target") == 0 && take_value(argc, argv, &i, &value, error)) {
            options->pool_target = atoi(value);
        } else if (strcmp(arg, "--health-interval") == 0 && take_value(argc, argv, &i, &value, error)) {
            options->health_interval_sec = atoi(value);
        } else if (strcmp(arg, "--health-timeout") == 0 && take_value(argc, argv, &i, &value, error)) {
            options->health_timeout_ms = atoi(value);
        } else if (strcmp(arg, "--health-url") == 0 && take_value(argc, argv, &i, &value, error)) {
            g_free(options->health_url);
            options->health_url = g_strdup(value);
        } else if (strcmp(arg, "--health-expected") == 0 && take_value(argc, argv, &i, &value, error)) {
            g_free(options->health_expected);
            options->health_expected = g_strdup(value);
        } else if (strcmp(arg, "--test-url") == 0 && take_value(argc, argv, &i, &value, error)) {
            g_free(options->test_url);
            options->test_url = g_strdup(value);
        } else if (strcmp(arg, "--test-expected") == 0 && take_value(argc, argv, &i, &value, error)) {
            g_free(options->test_expected);
            options->test_expected = g_strdup(value);
        } else if (strcmp(arg, "--screenshot") == 0 && take_value(argc, argv, &i, &value, error)) {
            g_free(options->screenshot_path);
            options->screenshot_path = g_strdup(value);
        } else if (strncmp(arg, "--", 2) == 0) {
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_UNKNOWN_OPTION,
                        "unknown option %s", arg);
            return false;
        } else {
            g_ptr_array_add(options->urls, g_strdup(arg));
        }
    }
    if (options->batch == 0) options->batch = 70;
    if (options->concurrency == 0) options->concurrency = 70;
    if (options->timeout_ms <= 0) options->timeout_ms = 4000;
    if (options->fetch_interval_sec <= 0) options->fetch_interval_sec = 3600;
    if (options->slow_interval_sec <= 0) options->slow_interval_sec = 24;
    if (options->pool_target <= 0) options->pool_target = 12;
    if (options->health_interval_sec <= 0) options->health_interval_sec = 6;
    if (options->health_timeout_ms <= 0) options->health_timeout_ms = 2500;
    return true;
}

int main(int argc, char** argv) {
    App app;
    memset(&app, 0, sizeof(app));
    app.sub_ids = g_array_new(FALSE, FALSE, sizeof(int64_t));

    GError* error = NULL;
    options_init(&app.options);
    if (!parse_options(argc, argv, &app.options, &error)) {
        fprintf(stderr, "%s\n", error != NULL ? error->message : "invalid arguments");
        g_clear_error(&error);
        options_free(&app.options);
        return 2;
    }

    mewnet_set_log_callback(on_mihomo_log, &app);
    mewnet_set_log_level(MEWNET_LOG_WARNING);

    GtkApplication* gtk_app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(gtk_app, "activate", G_CALLBACK(on_activate), &app);
    g_unix_signal_add(SIGINT, on_unix_signal, &app);
    g_unix_signal_add(SIGTERM, on_unix_signal, &app);

    char* gtk_argv[] = {argv[0], NULL};
    int status = g_application_run(G_APPLICATION(gtk_app), 1, gtk_argv);

    cleanup(&app);
    g_object_unref(gtk_app);
    g_array_unref(app.sub_ids);
    options_free(&app.options);
    return status;
}
