/* Complete production host linkage; only tmux.c's entrypoint is renamed. */
#include "../tmux.h"
#include "test-harness.h"
#include <sys/socket.h>
#include <unistd.h>

static struct event_base *base;

static void
setup(void)
{
	const struct options_table_entry *oe;

	base = event_init();
	TAILQ_INIT(&message_log);
	TAILQ_INIT(&clients);
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

/* Real socket-backed control output; no control/monitor/parser stubs. */
struct fixture {
	struct client c;
	int input[2], output[2];
};

static void
start(struct fixture *f, struct session *s)
{
	memset(f, 0, sizeof *f);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, f->input) != 0 ||
	    socketpair(AF_UNIX, SOCK_STREAM, 0, f->output) != 0)
		fatal("socketpair");
	f->c.name = "control-test";
	f->c.references = 1;
	f->c.flags = CLIENT_CONTROL|CLIENT_ATTACHED;
	f->c.session = s;
	f->c.fd = f->input[0];
	f->c.out_fd = f->output[0];
	f->c.queue = cmdq_new();
	f->c.environ = environ_create();
	setblocking(f->output[1], 0);
	control_start(&f->c);
	TAILQ_INSERT_TAIL(&clients, &f->c, entry);
}

static void
stop(struct fixture *f)
{
	control_stop(&f->c);
	control_stop(&f->c); /* Pinned stop is safe after an earlier stop. */
	TAILQ_REMOVE(&clients, &f->c, entry);
	cmdq_free(f->c.queue);
	environ_free(f->c.environ);
	close(f->input[0]);
	close(f->input[1]);
	close(f->output[0]);
	close(f->output[1]);
}

static char *
output(struct fixture *f)
{
	static char buffer[8192];
	ssize_t n;
	size_t used = 0;

	event_base_loop(base, EVLOOP_NONBLOCK);
	while ((n = read(f->output[1], buffer + used,
	    sizeof buffer - used - 1)) > 0)
		used += n;
	buffer[used] = '\0';
	return (buffer);
}

/* Commands traverse the actual parser, client queue and target resolution. */
static char *
command(struct fixture *f, const char *text)
{
	struct cmd_parse_result *pr = cmd_parse_from_string(text, NULL);
	struct cmdq_item *item;

	if (pr->status != CMD_PARSE_SUCCESS)
		fatalx("test command did not parse: %s", text);
	item = cmdq_get_command(pr->cmdlist, NULL);
	cmd_list_free(pr->cmdlist);
	f->c.retval = 0;
	cmdq_append(&f->c, item);
	while (cmdq_next(&f->c) != 0)
		;
	drain();
	return (output(f));
}

TEST(display_and_introspection)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	char *expected;
	char *text;

	start(&f, s);
	xasprintf(&text, "set-hook -g -B '@watch-session::value'");
	(void)command(&f, text);
	free(text);
	xasprintf(&text, "set-hook -p -B '@watch-pane:%%%u:value'", wp->id);
	(void)command(&f, text);
	free(text);
	xasprintf(&text, "show-hooks -g -B -F '#{option_name}:#{hook_monitor_target}:#{hook_monitor_format}:#{hook_monitor_display}'");
	ASSERT(strstr(command(&f, text),
	    "@watch-session::value:@watch-session::value") != NULL);
	free(text);
	xasprintf(&text, "show-hooks -p -B -F '#{option_name}:#{hook_monitor_target}:#{hook_monitor_format}:#{hook_monitor_display}'");
	xasprintf(&expected, "@watch-pane:%%%u:value:@watch-pane:%%%u:value",
	    wp->id, wp->id);
	ASSERT(strstr(command(&f, text), expected) != NULL);
	free(expected);
	free(text);
	xasprintf(&text, "set-hook -gu -B @watch-session");
	(void)command(&f, text);
	free(text);
	xasprintf(&text, "set-hook -pu -B @watch-pane");
	(void)command(&f, text);
	free(text);
	stop(&f);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(command_monitor_changes_replacement_and_removal)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	struct options_entry *o;
	int refs = s->references;

	start(&f, s);
	options_set_string(s->options, "@command-value", 0, "one");
	command(&f, "set-hook -B '@command-watch::#{@command-value}' { set-option -g @command-result '#{hook_value}/#{hook_last}' }");
	ASSERT_EQ(f.c.retval, 0);
	o = options_get_only(s->options, "@command-watch");
	ASSERT_NOT_NULL(o);
	ASSERT_NOT_NULL(options_get_monitor_data(o));
	ASSERT_EQ(s->references, refs + 1);
	tick(); /* Pinned command omits initial delivery. */
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 0);
	ASSERT(options_get_only(global_s_options, "@command-result") == NULL);
	options_set_string(s->options, "@command-value", 0, "two");
	tick();
	ASSERT_STR_EQ(options_get_string(global_s_options, "@command-result"), "two/one");
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 1);
	command(&f, "set-hook -B '@command-watch::#{@command-value}'");
	ASSERT_EQ(s->references, refs + 1);
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 0);
	tick();
	options_set_string(s->options, "@command-value", 0, "0");
	tick();
	ASSERT_STR_EQ(options_get_string(global_s_options, "@command-result"), "0/two");
	command(&f, "set-hook -u -B '@command-watch'");
	ASSERT(options_get_monitor_data(o) == NULL);
	ASSERT_EQ(s->references, refs);
	ASSERT_NOT_NULL(options_get_only(s->options, "@command-watch"));
	options_set_string(s->options, "@command-value", 0, "after");
	tick();
	ASSERT_STR_EQ(options_get_string(global_s_options, "@command-result"), "0/two");
	ASSERT_EQ(f.c.references, 1);
	stop(&f);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(command_monitor_true_append_scope_and_option_cleanup)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	struct options_entry *o;
	int refs = s->references;

	start(&f, s);
	options_set_string(s->options, "@true-value", 0, "one");
	command(&f, "set-hook -T -B '@true-watch::#{@true-value}' 'set-option -g @true-result #{hook_value}'");
	o = options_get_only(s->options, "@true-watch");
	tick();
	options_set_string(s->options, "@true-value", 0, "0");
	tick();
	ASSERT_EQ(hooks_monitor_get_fire_count(o), 0);
	options_set_string(s->options, "@true-value", 0, "yes");
	tick();
	ASSERT_STR_EQ(options_get_string(global_s_options, "@true-result"), "yes");
	command(&f, "set-hook -a -T -B '@true-watch::#{@true-value}' ' ; set-option -g @append-result yes'");
	tick();
	options_set_string(s->options, "@true-value", 0, "next");
	tick();
	ASSERT_STR_EQ(options_get_string(global_s_options, "@append-result"), "yes");
	command(&f, "set-option -u @true-watch"); /* Option removal owns cleanup. */
	ASSERT_EQ(s->references, refs);
	ASSERT(options_get_only(s->options, "@true-watch") == NULL);
	command(&f, "set-hook -g -B '@global-watch::constant'");
	ASSERT_EQ(s->references, refs); /* Global monitors do not retain target. */
	ASSERT_NOT_NULL(options_get_monitor_data(options_get_only(global_s_options, "@global-watch")));
	command(&f, "set-hook -gu -B '@global-watch::constant'");
	ASSERT(options_get_monitor_data(options_get_only(global_s_options, "@global-watch")) == NULL);
	stop(&f);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(command_monitor_validation_errors_preserve_ownership)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	struct options_entry *o;
	int refs;
	const char *invalid[] = {
	    "set-hook -B '@invalid:unknown:value'",
	    "set-hook -B 'plain::value'",
	    "set-hook -B '@invalid::value' one two",
	    "set-hook",
	    "set-hook -E",
	    "set-hook -E plain",
	    "set-hook -E @invalid extra"
	};
	const char *errors[] = { "invalid subscription", "monitor hook name must start with @",
	    "too many arguments", "missing argument", "missing argument",
	    "event name must start with @", "too many arguments" };
	u_int i;

	start(&f, s);
	command(&f, "set-hook -B '@valid::constant'");
	o = options_get_only(s->options, "@valid");
	refs = s->references;
	for (i = 0; i < nitems(invalid); i++) {
		ASSERT(strstr(command(&f, invalid[i]), errors[i]) != NULL);
		ASSERT_EQ(f.c.retval, 1);
		ASSERT_EQ(s->references, refs);
		ASSERT_NOT_NULL(options_get_monitor_data(o));
		ASSERT(options_get_only(s->options, "@invalid") == NULL);
	}
	command(&f, "set-hook -u -B '@valid::constant'");
	stop(&f);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(command_pane_window_scopes_and_legacy_numeric_hooks)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	struct options_entry *pane, *window, *legacy;
	int refs = s->references;
	u_int fires;
	char *text;

	start(&f, s);
	xasprintf(&text, "set-hook -p -B '@pane-command:%%%u:#{pane_title}'", wp->id);
	command(&f, text);
	free(text);
	pane = options_get_only(wp->options, "@pane-command");
	ASSERT_NOT_NULL(options_get_monitor_data(pane));
	xasprintf(&text, "set-hook -w -B '@window-command:@%u:#{window_name}'", w->id);
	command(&f, text);
	free(text);
	window = options_get_only(w->options, "@window-command");
	ASSERT_NOT_NULL(options_get_monitor_data(window));
	ASSERT_EQ(s->references, refs + 2);
	command(&f, "set-hook -pu -B @pane-command");
	command(&f, "set-hook -wu -B @window-command");
	ASSERT(options_get_monitor_data(pane) == NULL);
	ASSERT(options_get_monitor_data(window) == NULL);
	ASSERT_EQ(s->references, refs);

	command(&f, "set-hook -g 'command-error[4]' { set-option -g @legacy-result yes }");
	legacy = options_get_only(global_s_options, "command-error");
	ASSERT_NOT_NULL(options_array_getv(legacy, "%u", 4));
	fires = options_get_fire_count(legacy);
	command(&f, "set-hook -gR command-error");
	ASSERT_STR_EQ(options_get_string(global_s_options, "@legacy-result"), "yes");
	ASSERT_EQ(options_get_fire_count(legacy), fires + 1);
	command(&f, "set-hook -gu 'command-error[4]'");
	ASSERT(options_array_getv(legacy, "%u", 4) == NULL);
	stop(&f);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(command_custom_event_executes_with_target_metadata)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	struct options_entry *o;
	char *expected;

	start(&f, s);
	command(&f, "set-hook @command-event { set-option -gF @event-result '#{hook}/#{hook_session}/#{hook_window}/#{hook_pane}' }");
	o = options_get_only(s->options, "@command-event");
	ASSERT_NOT_NULL(o);
	ASSERT_EQ(options_get_fire_count(o), 0);
	command(&f, "set-hook -E @command-event");
	ASSERT_EQ(f.c.retval, 0);
	xasprintf(&expected, "@command-event/$%u/@%u/%%%u", s->id, w->id, wp->id);
	ASSERT_STR_EQ(options_get_string(global_s_options, "@event-result"), expected);
	free(expected);
	ASSERT_EQ(options_get_fire_count(o), 1);
	command(&f, "set-hook -u @command-event");
	command(&f, "set-hook -E @command-event");
	ASSERT_EQ(f.c.retval, 0);
	ASSERT_EQ(f.c.references, 1);
	stop(&f);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(built_in_startup_hooks_and_control_notices)
{
	struct window *w1, *w2;
	struct window_pane *wp1, *wp2;
	struct session *s1 = new_session(&w1, &wp1);
	struct session *s2 = new_session(&w2, &wp2);
	struct fixture f;
	struct options_entry *o;

	options_array_set(options_get_only(global_s_options, "session-created"),
	    "0", "set-option -g @startup-hook yes", 0, NULL);
	drain();
	hooks_build_events();
	control_build_events();
	ASSERT(hooks_is_event("session-created"));
	start(&f, s1);
	events_fire_session("session-created", s1);
	drain();
	ASSERT_STR_EQ(output(&f), "%sessions-changed\n");
	o = options_get_only(global_s_options, "session-created");
	ASSERT_NOT_NULL(o);
	ASSERT_EQ(options_get_fire_count(o), 1);
	ASSERT_STR_EQ(options_get_string(global_s_options, "@startup-hook"), "yes");
	options_remove_or_default(o, NULL, NULL);
	stop(&f);
	session_destroy(s1, 0, __func__);
	drain();
	session_destroy(s2, 0, __func__);
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
	ASSERT_EQ(options_remove_or_default(o, NULL, NULL), 0);
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

static struct monitor_set	*callback_ms;
static void
mutating_change(struct monitor_change *change, void *data)
{
	(void)data;

	reports++;
	if (strcmp(change->name, "aaa") != 0)
		return;
	monitor_remove(callback_ms, "zzz");
}

TEST(change_callbacks_add_and_remove_subscriptions)
{
	struct window			*w;
	struct window_pane		*wp;
	struct session			*s = new_session(&w, &wp);
	struct monitor_set		*ms =
	    monitor_create_session(s, mutating_change, NULL);

	callback_ms = ms;
	reports = 0;
	monitor_add(ms, "aaa", MONITOR_SESSION, -1, "one",
	    MONITOR_NOTIFY_INITIAL);
	monitor_add(ms, "zzz", MONITOR_SESSION, -1, "one",
	    MONITOR_NOTIFY_INITIAL);
	tick();
	ASSERT_EQ(reports, 1);
	ASSERT_EQ(monitor_get_fire_count(ms, "aaa"), 1);
	ASSERT_EQ(monitor_get_fire_count(ms, "zzz"), 0);
	tick();
	ASSERT_EQ(reports, 1);

	monitor_remove(ms, "aaa");
	monitor_destroy(ms);
	session_destroy(s, 0, __func__);
	drain();
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
	struct message_entry *msg;
	setup();
	TEST_SUITE_BEGIN("Production monitor ownership and hook lifecycle");
	RUN_TEST(command_monitor_changes_replacement_and_removal);
	RUN_TEST(command_monitor_true_append_scope_and_option_cleanup);
	RUN_TEST(command_monitor_validation_errors_preserve_ownership);
	RUN_TEST(command_pane_window_scopes_and_legacy_numeric_hooks);
	RUN_TEST(command_custom_event_executes_with_target_metadata);
	RUN_TEST(built_in_startup_hooks_and_control_notices);
	RUN_TEST(session_hook_executes_changes_and_filters_false);
	RUN_TEST(replacement_inheritance_option_removal_and_free);
	RUN_TEST(dead_session_retained_until_monitor_destroy);
	RUN_TEST(session_destruction_breaks_option_monitor_cycle);
	RUN_TEST(pane_window_scans_and_subscription_replacement);
	RUN_TEST(change_callbacks_add_and_remove_subscriptions);
	RUN_TEST(display_and_introspection);
	drain();
	while ((msg = TAILQ_FIRST(&message_log)) != NULL) {
		TAILQ_REMOVE(&message_log, msg, entry);
		free(msg->msg);
		free(msg);
	}
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	free((void *)socket_path);
	event_base_free(base);
	TEST_SUITE_END();
}
