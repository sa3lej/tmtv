/* Link the complete production host; only tmux.c's entrypoint is renamed. */
#include "../tmux.h"
#include "test-harness.h"
#include <limits.h>

static int freed;

static void
free_pointer(void *p)
{
	freed++;
	free(p);
}

static void
print_pointer(void *p, struct evbuffer *evb)
{
	evbuffer_add_printf(evb, "pointer:%s", (char *)p);
}

static void
setup(void)
{
	const struct options_table_entry *oe;

	event_init();
	socket_path = xstrdup("/isolated-payload-test");
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

static void drain(void);

static struct session *
new_session(struct window **w, struct window_pane **wp)
{
	struct session *s;
	struct winlink *wl;

	s = session_create(NULL, "payload-test", "/", environ_create(),
	    options_create(global_s_options), NULL);
	*w = window_create(80, 24, 0, 0);
	*wp = window_add_pane(*w, NULL, 100, 0);
	window_set_active_pane(*w, *wp, 0);
	/* Build the fixture with actual production allocators and reference APIs. */
	wl = winlink_add(&s->windows, 4);
	wl->session = s;
	winlink_set_window(wl, *w);
	s->curw = wl;
	window_set_name(*w, "payload-window");
	drain();
	return (s);
}

static void
drain(void)
{
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
}

TEST(typed_items_formats_and_owned_pointer_replacement)
{
	struct event_payload *ep = event_payload_create();
	struct event_payload_item *item;
	struct format_tree *ft = format_create(NULL, NULL, 0, FORMAT_NOJOBS);
	char *text;
	int n = 17, count = 0;
	u_int un = 0;

	freed = 0;
	event_payload_set_string(ep, "message", "pane:%d", 42);
	event_payload_set_string(ep, "_private", "%s", "hidden");
	event_payload_set_int(ep, "number", INT_MIN);
	event_payload_set_uint(ep, "unsigned", UINT_MAX);
	event_payload_set_time(ep, "time", 123456);
	event_payload_set_pointer(ep, "owned", xstrdup("old"), free_pointer,
	    print_pointer);
	event_payload_set_pointer(ep, "owned", xstrdup("new"), free_pointer,
	    print_pointer);
	ASSERT_EQ(freed, 1);
	ASSERT_STR_EQ(event_payload_get_string(ep, "message"), "pane:42");
	ASSERT(event_payload_get_string(ep, "number") == NULL);
	ASSERT_EQ(event_payload_get_int(ep, "missing", &n), -1);
	ASSERT_EQ(n, 17);
	ASSERT_EQ(event_payload_get_int(ep, "number", &n), 0);
	ASSERT_EQ(n, INT_MIN);
	ASSERT_EQ(event_payload_get_uint(ep, "unsigned", &un), 0);
	ASSERT_EQ(un, UINT_MAX);
	ASSERT_EQ(event_payload_get_time(ep, "time"), 123456);
	ASSERT(event_payload_print(ep, "missing") == NULL);
	event_payload_add_formats(ep, ft, "hook_");
	text = format_expand(ft, "#{hook_message}/#{hook_owned}/#{hook__private}");
	ASSERT_STR_EQ(text, "pane:42/pointer:new/");
	free(text);
	for (item = event_payload_first(ep); item != NULL;
	    item = event_payload_next(item)) {
		ASSERT_NOT_NULL(event_payload_item_name(item));
		text = event_payload_item_print(item);
		ASSERT_NOT_NULL(text);
		free(text);
		count++;
	}
	ASSERT_EQ(count, 6);
	event_payload_set_string(ep, "owned", "%s", "replaced");
	ASSERT_EQ(freed, 2);
	event_payload_free(ep);
	ASSERT_EQ(freed, 2);
	event_payload_free(NULL);
	format_free(ft);
}

TEST(pane_item_survives_whole_window_and_releases_on_replacement)
{
	struct window *w = window_create(80, 24, 0, 0);
	struct window_pane *wp = window_add_pane(w, NULL, 100, 0);
	struct event_payload *ep = event_payload_create();
	char expected[32], *text;
	u_int id = wp->id;

	window_add_ref(w, __func__);
	event_payload_set_pane(ep, "pane", wp);
	event_payload_set_pane(ep, "pane", wp);
	ASSERT_EQ(wp->references, 2);
	window_remove_ref(w, __func__);
	ASSERT(window_pane_find_by_id(id) == NULL);
	ASSERT(wp->flags & PANE_DESTROYED);
	ASSERT_EQ(wp->references, 1);
	ASSERT(event_payload_get_pane(ep, "pane") == wp);
	snprintf(expected, sizeof expected, "%%%u", id);
	text = event_payload_print(ep, "pane");
	ASSERT_STR_EQ(text, expected);
	free(text);
	event_payload_set_int(ep, "pane", 9); /* Last pane reference is released. */
	ASSERT(event_payload_get_pane(ep, "pane") == NULL);
	event_payload_free(ep);
}

TEST(target_retention_replacement_and_dead_session_recovery)
{
	struct window *w;
	struct window_pane *wp;
	struct session *s = new_session(&w, &wp);
	struct event_payload *ep = event_payload_create();
	struct cmd_find_state target, found;
	int sr = s->references, wr = w->references;
	u_int id = wp->id;

	cmd_find_clear_state(&target, 0);
	target.wl = s->curw; /* Infer session and window from the winlink. */
	target.wp = wp;
	event_payload_set_target(ep, &target);
	ASSERT_EQ(s->references, sr + 1);
	ASSERT_EQ(w->references, wr + 1);
	ASSERT_EQ(wp->references, 2);
	event_payload_set_target(ep, &target);
	ASSERT_EQ(s->references, sr + 1);
	ASSERT_EQ(w->references, wr + 1);
	cmd_find_clear_state(&found, CMD_FIND_QUIET);
	ASSERT_EQ(event_payload_get_target(ep, &found), 1);
	ASSERT(cmd_find_valid_state(&found));
	ASSERT(found.s == s && found.w == w && found.wp == wp);
	ASSERT_EQ(found.idx, 4);
	ASSERT_EQ(found.flags, CMD_FIND_QUIET);
	session_destroy(s, 0, __func__);
	drain();
	ASSERT(!session_alive(s));
	ASSERT_EQ(event_payload_get_target(ep, &found), 0);
	ASSERT(found.s == NULL && found.w == NULL && found.wp == NULL);
	/* Release window first: destroys owner pane reference, then payload's. */
	event_payload_free(ep);
	drain();
	ASSERT(window_pane_find_by_id(id) == NULL);
}

TEST(removed_target_pane_recovers_to_live_pane)
{
	struct window *w;
	struct window_pane *wp, *other;
	struct session *s = new_session(&w, &wp);
	struct event_payload *ep = event_payload_create();
	struct cmd_find_state target, found, empty;

	other = window_add_pane(w, wp, 100, 0);
	cmd_find_from_winlink_pane(&target, s->curw, wp, 0);
	event_payload_set_target(ep, &target);
	window_set_active_pane(w, other, 0);
	window_remove_pane(w, wp);
	cmd_find_clear_state(&found, CMD_FIND_QUIET);
	ASSERT_EQ(event_payload_get_target(ep, &found), 1);
	ASSERT(cmd_find_valid_state(&found));
	ASSERT(found.wp == other);
	ASSERT(wp->flags & PANE_DESTROYED);
	cmd_find_clear_state(&empty, 0);
	event_payload_set_target(ep, &empty); /* Release removed pane now. */
	ASSERT_EQ(w->references, 1);
	event_payload_free(ep);
	session_destroy(s, 0, __func__);
	drain();
}

int
main(void)
{
	setup();
	TEST_SUITE_BEGIN("Production event payloads");
	RUN_TEST(typed_items_formats_and_owned_pointer_replacement);
	RUN_TEST(pane_item_survives_whole_window_and_releases_on_replacement);
	RUN_TEST(target_retention_replacement_and_dead_session_recovery);
	RUN_TEST(removed_target_pane_recovers_to_live_pane);
	drain();
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	free((void *)socket_path);
	TEST_SUITE_END();
}
