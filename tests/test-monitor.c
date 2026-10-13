/* Complete production host linkage; only tmux.c's entrypoint is renamed. */
#include "../tmux.h"
#include "test-harness.h"

static struct event_base *base;

static void
setup(void)
{
	const struct options_table_entry *oe;

	base = event_init();
	socket_path = xstrdup("/isolated-monitor-test");
	global_environ = environ_create();
	global_options = options_create(NULL);
	global_s_options = options_create(NULL);
	global_w_options = options_create(NULL);
	for (oe = options_table; oe->name != NULL; oe++) {
		if (oe->scope & OPTIONS_TABLE_SERVER)
			options_default(global_options, oe);
		if (oe->scope & OPTIONS_TABLE_SESSION)
			options_default(global_s_options, oe);
		if (oe->scope & OPTIONS_TABLE_WINDOW)
			options_default(global_w_options, oe);
	}
}

static void
drain(void)
{
	while (cmdq_next(NULL) != 0)
		;
	event_base_loop(base, EVLOOP_NONBLOCK);
}

static struct session *
new_session(struct window **w, struct window_pane **wp)
{
	struct session *s;
	struct winlink *wl;

	s = session_create(NULL, "monitor-test", "/", environ_create(),
	    options_create(global_s_options), NULL);
	*w = window_create(80, 24, 0, 0);
	*wp = window_add_pane(*w, NULL, 100, 0);
	window_set_active_pane(*w, *wp, 0);
	/* Build the fixture with actual production allocators and reference APIs. */
	wl = winlink_add(&s->windows, 4);
	wl->session = s;
	winlink_set_window(wl, *w);
	s->curw = wl;
	window_set_name(*w, "monitor-window");
	drain();
	return (s);
}


static void
tick(void)
{
	struct timeval limit = { .tv_sec = 1, .tv_usec = 100000 };

	if (event_base_loopexit(base, &limit) != 0)
		fatalx("timer bound failed");
	event_base_loop(base, 0);
	drain();
}

TEST(session_hook_executes_changes_and_filters_false)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct cmd_find_state fs;
	struct options_entry *o;
	const char *format;
	enum monitor_type type;
	int id, refs = s->references;
	char *text;

	cmd_find_from_session(&fs, s, 0);
	o = options_set_string(s->options, "@watch", 0,
	    "set-option -g @monitor-result '#{hook_value}/#{hook_last}'");
	options_set_string(s->options, "@watched", 0, "one");
	hooks_monitor_add(NULL, s->options, "@watch", MONITOR_SESSION, -1,
	    "#{@watched}", MONITOR_NOTIFY_INITIAL | MONITOR_NOTIFY_TRUE, &fs, s);
	ASSERT_EQ(s->references, refs + 1);
	ASSERT_NOT_NULL(options_get_monitor_data(o));
	ASSERT_EQ(hooks_monitor_get(o, &type, &id, &format), 1);
	ASSERT_EQ(type, MONITOR_SESSION);
	ASSERT_EQ(id, -1);
	ASSERT_STR_EQ(format, "#{@watched}");
	text = hooks_monitor_to_string(o);
	ASSERT_STR_EQ(text, "@watch::#{@watched}");
	free(text);
	current_time = 4321;
	tick();
	ASSERT_STR_EQ(options_get_string(global_s_options, "@monitor-result"), "one/");
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 1);
	ASSERT_EQ(hooks_monitor_get_fire_time(o), 4321);
	ASSERT_EQ(options_get_fire_count(o), 1);
	tick();
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 1);
	options_set_string(s->options, "@watched", 0, "0");
	tick();
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 1);
	options_set_string(s->options, "@watched", 0, "two");
	tick();
	ASSERT_STR_EQ(options_get_string(global_s_options, "@monitor-result"), "two/0");
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 2);
	ASSERT_EQ(options_get_fire_count(o), 2);
	hooks_monitor_remove(s->options, "@watch");
	ASSERT(options_get_monitor_data(o) == NULL);
	ASSERT_EQ(s->references, refs);
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 0);
	session_destroy(s, 0, __func__);
	drain();
}

static int reports;
static void
observe(const char *name, struct event_payload *ep, void *data)
{
	(void)name;
	(void)ep;
	(void)data;
	reports++;
}

TEST(replacement_inheritance_option_removal_and_free)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct cmd_find_state fs;
	struct options *child = options_create(s->options);
	struct options_entry *o;
	struct events_sink *sink = events_add_sink("@owned", observe, NULL);
	int refs = s->references;

	reports = 0;
	cmd_find_from_session(&fs, s, 0);
	o = options_set_string(s->options, "@owned", 0, "%s", "");
	hooks_monitor_add(NULL, s->options, "@owned", MONITOR_SESSION, -1,
	    "first", MONITOR_NOTIFY_INITIAL, &fs, s);
	ASSERT(options_get(child, "@owned") == o);
	hooks_monitor_remove(child, "@owned"); /* Must not remove parent ownership. */
	ASSERT_NOT_NULL(options_get_monitor_data(o));
	hooks_monitor_add(NULL, s->options, "@owned", MONITOR_SESSION, -1,
	    "replacement", MONITOR_NOTIFY_INITIAL, &fs, s);
	ASSERT_EQ(s->references, refs + 1);
	tick();
	ASSERT_EQ(reports, 1);
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 1);
	ASSERT_EQ(options_remove_or_default(o, -1, NULL), 0);
	ASSERT_EQ(s->references, refs);
	tick();
	ASSERT_EQ(reports, 1); /* Removed option cancels its timer and sink. */
	hooks_monitor_add(NULL, child, "@owned", MONITOR_SESSION, -1,
	    "child", MONITOR_NOTIFY_INITIAL, &fs, s);
	ASSERT_EQ(s->references, refs + 1);
	options_free(child);
	ASSERT_EQ(s->references, refs);
	tick();
	ASSERT_EQ(reports, 1);
	events_remove_sink(sink);
	session_destroy(s, 0, __func__);
	drain();
}

static void
count_change(struct monitor_change *change, void *data)
{
	(void)change;
	(void)data;
	reports++;
}

TEST(dead_session_retained_until_monitor_destroy)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct monitor_set *ms = monitor_create_session(s, count_change, NULL);
	u_int id = s->id;

	reports = 0;
	monitor_add(ms, "dead", MONITOR_SESSION, -1, "alive", MONITOR_NOTIFY_INITIAL);
	tick();
	ASSERT_EQ(reports, 1);
	session_destroy(s, 0, __func__);
	drain();
	ASSERT(session_find_by_id(id) == NULL);
	ASSERT_EQ(s->references, 1);
	tick();
	ASSERT_EQ(reports, 1);
	monitor_destroy(ms);
	drain();
	monitor_destroy(NULL);
}

TEST(session_destruction_breaks_option_monitor_cycle)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct monitor_set *anchor = monitor_create_session(s, count_change, NULL);
	struct cmd_find_state fs;
	struct options_entry *o;

	cmd_find_from_session(&fs, s, 0);
	hooks_monitor_add(NULL, s->options, "@cycle", MONITOR_SESSION, -1,
	    "value", MONITOR_NOTIFY_INITIAL, &fs, s);
	o = options_get_only(s->options, "@cycle");
	ASSERT_NOT_NULL(options_get_monitor_data(o));
	session_destroy(s, 0, __func__);
	drain();
	ASSERT_EQ(s->references, 1); /* Only the external anchor remains. */
	ASSERT(options_get_monitor_data(o) == NULL);
	tick();
	ASSERT_EQ(options_get_fire_count(o), 0);
	monitor_destroy(anchor);
	drain();
}

TEST(pane_window_scans_and_subscription_replacement)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct monitor_set *ms = monitor_create_session(s, count_change, NULL);

	reports = 0;
	monitor_add(ms, "panes", MONITOR_ALL_PANES, -1, "#{pane_title}", MONITOR_NOTIFY_INITIAL);
	monitor_add(ms, "windows", MONITOR_ALL_WINDOWS, -1, "#{window_name}", MONITOR_NOTIFY_INITIAL);
	tick();
	ASSERT_EQ(reports, 2);
	window_set_name(w, "changed");
	tick();
	ASSERT_EQ(reports, 3);
	ASSERT_EQ(monitor_get_fire_count(ms, "windows"), 2);
	monitor_add(ms, "windows", MONITOR_WINDOW, w->id, "#{window_name}", MONITOR_NOTIFY_INITIAL);
	ASSERT_EQ(monitor_get_fire_count(ms, "windows"), 0);
	tick();
	ASSERT_EQ(reports, 4);
	monitor_remove(ms, "windows");
	monitor_remove(ms, "panes");
	ASSERT_EQ(monitor_get_fire_count(ms, "windows"), 0);
	tick();
	ASSERT_EQ(reports, 4);
	monitor_destroy(ms);
	session_destroy(s, 0, __func__);
	drain();
}

int
main(void)
{
	setup();
	TEST_SUITE_BEGIN("Production monitor ownership and hook lifecycle");
	RUN_TEST(session_hook_executes_changes_and_filters_false);
	RUN_TEST(replacement_inheritance_option_removal_and_free);
	RUN_TEST(dead_session_retained_until_monitor_destroy);
	RUN_TEST(session_destruction_breaks_option_monitor_cycle);
	RUN_TEST(pane_window_scans_and_subscription_replacement);
	drain();
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	free((void *)socket_path);
	event_base_free(base);
	TEST_SUITE_END();
}
