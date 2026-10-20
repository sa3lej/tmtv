/* Complete production host linkage; only tmux.c's entrypoint is renamed. */
#include "../tmux.h"
#include "test-harness.h"
#include <sys/socket.h>
#include <string.h>
#include <unistd.h>

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

	s = session_create(NULL, NULL, "/", environ_create(),
	    options_create(global_s_options), NULL);
	*w = window_create(80, 24, 0, 0);
	*wp = window_add_pane(*w, NULL, 100, 0);
	window_set_active_pane(*w, *wp, 0);
	/* Build the fixture with actual production allocators and reference APIs. */
	wl = winlink_add(&s->windows, 4);
	wl->session = s;
	winlink_set_window(wl, *w);
	s->curw = wl;
	window_set_name(*w, "monitor-window", 0);
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
}

static void
stop(struct fixture *f)
{
	control_stop(&f->c);
	control_stop(&f->c); /* Pinned stop is safe after an earlier stop. */
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
	static char buffer[32768];
	ssize_t n;
	size_t used = 0;

	event_base_loop(base, EVLOOP_NONBLOCK);
	while ((n = read(f->output[1], buffer + used,
	    sizeof buffer - used - 1)) > 0)
		used += n;
	buffer[used] = '\0';
	return (buffer);
}

static void
run(struct fixture *f, const char *command)
{
	struct cmd_parse_result *pr;
	struct cmdq_item *item;
	char *text;

	xasprintf(&text, "%s", command);
	pr = cmd_parse_from_string(text, NULL);
	free(text);
	if (pr->status != CMD_PARSE_SUCCESS)
		fatalx("control command parse failed");
	item = cmdq_get_command(pr->cmdlist, NULL);
	cmd_list_free(pr->cmdlist);
	cmdq_append(&f->c, item);
	while (cmdq_next(&f->c) != 0)
		;
}

static void
refresh(struct fixture *f, const char *value)
{
	struct cmd_parse_result *pr;
	struct cmdq_item *item;
	char *text;

	xasprintf(&text, "refresh-client -B '%s'", value);
	pr = cmd_parse_from_string(text, NULL);
	free(text);
	if (pr->status != CMD_PARSE_SUCCESS)
		fatalx("refresh parse failed");
	item = cmdq_get_command(pr->cmdlist, NULL);
	cmd_list_free(pr->cmdlist);
	cmdq_append(&f->c, item);
	while (cmdq_next(&f->c) != 0)
		;
	(void)output(f); /* Discard command reply guards. */
}

TEST(session_output_changes_replacement_and_removal)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	char *actual, *expected;

	start(&f, s);
	options_set_string(s->options, "@value", 0, "one");
	refresh(&f, "watch::#{@value}");
	tick();
	xasprintf(&expected, "%%subscription-changed watch $%u - - - : one\n", s->id);
	actual = output(&f);
	ASSERT_STR_EQ(actual, expected);
	free(expected);
	tick();
	actual = output(&f);
	ASSERT_STR_EQ(actual, "");
	options_set_string(s->options, "@value", 0, "0");
	tick();
	xasprintf(&expected, "%%subscription-changed watch $%u - - - : 0\n", s->id);
	actual = output(&f);
	ASSERT_STR_EQ(actual, expected);
	free(expected);
	refresh(&f, "watch::replacement");
	tick();
	xasprintf(&expected, "%%subscription-changed watch $%u - - - : replacement\n", s->id);
	actual = output(&f);
	ASSERT_STR_EQ(actual, expected);
	free(expected);
	refresh(&f, "watch");
	tick();
	actual = output(&f);
	ASSERT_STR_EQ(actual, "");
	ASSERT_EQ(f.c.references, 1);
	stop(&f);
	ASSERT(f.c.control_state == NULL);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(pane_window_scopes_and_session_isolation)
{
	struct window *w, *other;
	struct window_pane *wp, *otherwp;
	struct session *s = new_session(&w, &wp);
	struct session *s2 = new_session(&other, &otherwp);
	struct fixture f;
	char *actual, *value, *expected;

	start(&f, s);
	xasprintf(&value, "foreign:@%u:#{window_name}", other->id);
	refresh(&f, value);
	free(value);
	refresh(&f, "panes:%*:#{pane_id}");
	refresh(&f, "windows:@*:#{window_name}");
	tick();
	xasprintf(&expected,
	    "%%subscription-changed panes $%u @%u 4 %%%u : %%%u\n"
	    "%%subscription-changed windows $%u @%u 4 - : monitor-window\n",
	    s->id, w->id, wp->id, wp->id, s->id, w->id);
	actual = output(&f);
	ASSERT_STR_EQ(actual, expected);
	free(expected);
	refresh(&f, "panes");
	refresh(&f, "windows");
	/* A client monitor follows its client's current session. */
	f.c.session = s2;
	tick();
	xasprintf(&expected, "%%subscription-changed foreign $%u @%u 4 - : monitor-window\n", s2->id, other->id);
	actual = output(&f);
	ASSERT_STR_EQ(actual, expected);
	free(expected);
	f.c.session = NULL;
	tick();
	actual = output(&f);
	ASSERT_STR_EQ(actual, "");
	stop(&f);
	session_destroy(s, 0, __func__);
	session_destroy(s2, 0, __func__);
	drain();
}

TEST(nested_guards_flush_after_outer_block)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	char *actual;

	start(&f, s);
	control_write_guard(&f.c, "begin", 1, 1, 1);
	control_write_guard(&f.c, "begin", 1, 2, 1);
	control_notify_write(&f.c, "%%deferred-notification");
	control_write_guard(&f.c, "end", 1, 2, 1);
	actual = output(&f);
	ASSERT(strstr(actual, "%deferred-notification") == NULL);
	control_write_guard(&f.c, "end", 1, 1, 1);
	actual = output(&f);
	ASSERT(strstr(actual, "%deferred-notification") != NULL);
	ASSERT(strstr(actual, "%deferred-notification") >
	    strstr(actual, "%end"));
	stop(&f);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(control_window_size_set_get_and_clear)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	u_int sx, sy;

	start(&f, s);
	control_set_window_size(&f.c, w->id, 40, 20);
	ASSERT_EQ(control_get_window_size(&f.c, w->id, &sx, &sy), 1);
	ASSERT_EQ(sx, 40);
	ASSERT_EQ(sy, 20);
	control_clear_window_size(&f.c, w->id);
	ASSERT_EQ(control_get_window_size(&f.c, w->id, &sx, &sy), 0);
	control_clear_window_size(&f.c, w->id);
	stop(&f);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(stopped_outer_guard_releases_deferred_line)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;

	start(&f, s);
	control_write_guard(&f.c, "begin", 1, 1, 1);
	control_notify_write(&f.c, "%%stop-deferred");
	stop(&f);
	(void)output(&f);
	ASSERT(f.c.control_state == NULL);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(reply_backpressure_marks_and_discards_client)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	u_int repeat;

	start(&f, s);
	/* 256 byte lines exceed the 64 MiB pinned reply limit, while keeping
	 * the test bounded. */
	for (repeat = 0; repeat < 400000; repeat++) {
		control_write(&f.c, "%0255d", 1);
		if (f.c.flags & CLIENT_CONTROL_DISCARD)
			break;
	}
	ASSERT(f.c.flags & CLIENT_CONTROL_DISCARD);
	ASSERT(f.c.flags & CLIENT_EXIT);
	control_discard_all(&f.c);
	stop(&f);
	free(f.c.exit_message);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(parser_rejects_unknown_scope_and_stop_cancels_pending_timer)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	char *actual;

	start(&f, s);
	/* Old parser treated arbitrary selectors as session subscriptions. */
	refresh(&f, "invalid:nonsense:leaked");
	tick();
	actual = output(&f);
	ASSERT_STR_EQ(actual, "");
	refresh(&f, "pending::never-written");
	stop(&f);
	tick(); /* Real pending monitor timer must not touch the freed state. */
	ASSERT(f.c.control_state == NULL);
	session_destroy(s, 0, __func__);
	drain();
}

TEST(single_pane_socket_command_and_closed_pane)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct fixture f;
	char *actual, *text, *expected;
	int pane[2];

	start(&f, s);
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pane) != 0)
		fatal("socketpair");
	wp->fd = pane[0];
	xasprintf(&text, "refresh-client -B 'single:%%%u:#{pane_id}'\n", wp->id);
	control_ready(&f.c);
	ASSERT_EQ(write(f.input[1], text, strlen(text)), (ssize_t)strlen(text));
	free(text);
	event_base_loop(base, EVLOOP_NONBLOCK);
	while (cmdq_next(&f.c) != 0)
		;
	(void)output(&f);
	tick();
	xasprintf(&expected, "%%subscription-changed single $%u @%u 4 %%%u : %%%u\n", s->id, w->id, wp->id, wp->id);
	actual = output(&f);
	ASSERT_STR_EQ(actual, expected);
	free(expected);
	/* Replace with a different value, then close the pane before checking. */
	xasprintf(&text, "single:%%%u:closed", wp->id);
	refresh(&f, text);
	free(text);
	close(wp->fd);
	wp->fd = -1;
	close(pane[1]);
	tick();
	actual = output(&f);
	ASSERT_STR_EQ(actual, "");
	stop(&f);
	session_destroy(s, 0, __func__);
	drain();
}

int
main(void)
{
	setup();
	TEST_SUITE_BEGIN("Production control subscriptions");
	RUN_TEST(session_output_changes_replacement_and_removal);
	RUN_TEST(pane_window_scopes_and_session_isolation);
	RUN_TEST(nested_guards_flush_after_outer_block);
	RUN_TEST(control_window_size_set_get_and_clear);
	RUN_TEST(stopped_outer_guard_releases_deferred_line);
	RUN_TEST(reply_backpressure_marks_and_discards_client);
RUN_TEST(parser_rejects_unknown_scope_and_stop_cancels_pending_timer);
	RUN_TEST(single_pane_socket_command_and_closed_pane);
	drain();
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	free((void *)socket_path);
	event_base_free(base);
	TEST_SUITE_END();
}
