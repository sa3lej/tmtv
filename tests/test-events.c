/* Complete production host linkage; only the program entrypoint is renamed. */
#include "../tmux.h"
#include "test-harness.h"

static int freed, calls, trace[16], count;
static struct events_sink *first, *second, *added;

static void
free_owned(void *p)
{
	freed++;
	free(p);
}

static struct event_payload *
payload(void)
{
	struct event_payload *ep = event_payload_create();
	event_payload_set_pointer(ep, "owned", xstrdup("value"), free_owned, NULL);
	return (ep);
}

static void
observe(const char *name, struct event_payload *ep, void *data)
{
	ASSERT_STR_EQ(event_payload_get_string(ep, "event"), name);
	ASSERT_STR_EQ((char *)event_payload_get_pointer(ep, "owned"), "value");
	calls++;
	trace[count++] = *(int *)data;
}

TEST(matching_order_and_payload_consumption)
{
	int a = 1, b = 2, c = 3;
	struct events_sink *other;

	freed = calls = count = 0;
	first = events_add_sink("test", observe, &a);
	other = events_add_sink("other", observe, &c);
	second = events_add_sink("test", observe, &b);
	events_fire("test", payload());
	ASSERT_EQ(calls, 2);
	ASSERT_EQ(trace[0], 1);
	ASSERT_EQ(trace[1], 2);
	ASSERT_EQ(freed, 1);
	events_remove_sink(first);
	events_remove_sink(second);
	events_remove_sink(other);
	events_remove_sink(NULL);
	events_fire("test", payload());
	ASSERT_EQ(calls, 2);
	ASSERT_EQ(freed, 2);
}

static void
remove_during_dispatch(const char *name, struct event_payload *ep, void *data)
{
	observe(name, ep, data);
	events_remove_sink(first);
	events_remove_sink(first); /* Idempotent while deferred. */
	events_remove_sink(second); /* Skip a not-yet-called sink. */
}

TEST(self_and_pending_sink_removal)
{
	int a = 1, b = 2;

	freed = calls = count = 0;
	first = events_add_sink("test", remove_during_dispatch, &a);
	second = events_add_sink("test", observe, &b);
	events_fire("test", payload());
	ASSERT_EQ(calls, 1);
	ASSERT_EQ(freed, 1);
	events_fire("test", payload());
	ASSERT_EQ(calls, 1);
	ASSERT_EQ(freed, 2);
}

static int nesting, added_id = 3;

static void
add_and_nest(const char *name, struct event_payload *ep, void *data)
{
	observe(name, ep, data);
	if (!nesting) {
		nesting = 1;
		added = events_add_sink(name, observe, &added_id);
		events_fire(name, payload());
		/* Dead until outer dispatch completes, including nested traversals. */
		events_remove_sink(second);
		events_fire(name, payload());
	}
}

TEST(nested_dispatch_registration_generation_and_deferred_removal)
{
	int a = 1, b = 2;
	int expected[] = {1, 1, 2, 3, 1, 3};
	u_int i;

	freed = calls = count = nesting = 0;
	first = events_add_sink("test", add_and_nest, &a);
	second = events_add_sink("test", observe, &b);
	events_fire("test", payload());
	ASSERT_EQ(count, 6);
	for (i = 0; i < nitems(expected); i++)
		ASSERT_EQ(trace[i], expected[i]);
	ASSERT_EQ(freed, 3);
	/* New sink participates in nested/new events, not original generation. */
	events_fire("test", payload());
	ASSERT_EQ(count, 8);
	ASSERT_EQ(trace[6], 1);
	ASSERT_EQ(trace[7], 3);
	ASSERT_EQ(freed, 4);
	events_remove_sink(first);
	events_remove_sink(added);
}

static void
pane_observer(const char *name, struct event_payload *ep, void *data)
{
	struct window_pane *wp = data;
	struct window *w = wp->window;

	ASSERT_STR_EQ(event_payload_get_string(ep, "event"), name);
	ASSERT(event_payload_get_pane(ep, "pane") == wp);
	ASSERT(event_payload_get_window(ep, "window") == w);
	ASSERT_EQ(wp->references, 2);
	ASSERT_EQ(w->references, 2);
	window_remove_pane(w, wp);
	ASSERT(wp->flags & PANE_DESTROYED);
	ASSERT_EQ(wp->references, 1);
	calls++;
}

TEST(pane_wrapper_retains_removed_pane_until_dispatch_finishes)
{
	struct window *w = window_create(80, 24, 0, 0);
	struct window_pane *wp = window_add_pane(w, NULL, 100, 0);
	u_int id = wp->id;

	window_add_ref(w, __func__);
	calls = 0;
	first = events_add_sink("pane-test", pane_observer, wp);
	events_fire_pane("pane-test", wp);
	ASSERT_EQ(calls, 1);
	ASSERT(window_pane_find_by_id(id) == NULL);
	ASSERT_EQ(w->references, 1);
	events_remove_sink(first);
	window_remove_ref(w, __func__);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
}

int
main(void)
{
	const struct options_table_entry *oe;

	event_init();
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
	TEST_SUITE_BEGIN("Production event dispatcher");
	RUN_TEST(matching_order_and_payload_consumption);
	RUN_TEST(self_and_pending_sink_removal);
	RUN_TEST(nested_dispatch_registration_generation_and_deferred_removal);
	RUN_TEST(pane_wrapper_retains_removed_pane_until_dispatch_finishes);
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	TEST_SUITE_END();
}
