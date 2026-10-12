/* No stubs: link the complete production host, replacing only its entrypoint. */
#include "../tmux.h"
#include "test-harness.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

static void
unexpected_timer(int fd, short events, void *data)
{
	(void)fd;
	(void)events;
	(void)data;
	abort();
}

static void
setup(void)
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
}

TEST(retained_pane_detaches_and_closes_resources)
{
	struct window *w = window_create(80, 24, 0, 0);
	struct window_pane *wp = window_add_pane(w, NULL, 100, 0);
	struct window_pane_resize *resize;
	struct timeval delay = {60, 0};
	u_int id = wp->id;
	int pair[2], pipefd[2], fd, output;

	window_add_ref(w, __func__);
	ASSERT_EQ(wp->references, 1);
	ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), 0);
	wp->fd = fd = pair[0];
	window_pane_set_event(wp);
	ASSERT_EQ(pipe(pipefd), 0);
	wp->pipe_fd = output = pipefd[1];
	wp->pipe_event = bufferevent_new(output, NULL, NULL, NULL, NULL);
	ASSERT_NOT_NULL(wp->pipe_event);
	resize = xcalloc(1, sizeof *resize);
	TAILQ_INSERT_TAIL(&wp->resize_queue, resize, entry);
	/* A resize sentinel must be cancelled; sync uses the production callback. */
	evtimer_set(&wp->resize_timer, unexpected_timer, wp);
	evtimer_add(&wp->resize_timer, &delay);
	screen_write_start_sync(wp);
	ASSERT(event_pending(&wp->resize_timer, EV_TIMEOUT, NULL));
	ASSERT(event_pending(&wp->sync_timer, EV_TIMEOUT, NULL));
	window_pane_add_ref(wp, __func__);
	window_pane_add_ref(wp, "second payload");
	window_remove_pane(w, wp);
	ASSERT_EQ(wp->references, 2);
	ASSERT(wp->flags & PANE_DESTROYED);
	ASSERT(window_pane_find_by_id(id) == NULL);
	ASSERT_EQ(window_count_panes(w), 0);
	ASSERT_EQ(wp->fd, -1);
	ASSERT_EQ(wp->pipe_fd, -1);
	ASSERT(wp->event == NULL && wp->pipe_event == NULL && wp->ictx == NULL);
	ASSERT_EQ(fcntl(fd, F_GETFD), -1);
	ASSERT_EQ(fcntl(output, F_GETFD), -1);
	ASSERT(!event_pending(&wp->resize_timer, EV_TIMEOUT, NULL));
	ASSERT(!event_pending(&wp->sync_timer, EV_TIMEOUT, NULL));
	ASSERT(TAILQ_EMPTY(&wp->resize_queue));
	ASSERT_NOT_NULL(wp->base.grid);
	ASSERT_NOT_NULL(wp->options);
	window_pane_remove_ref(wp, "second payload");
	ASSERT_EQ(wp->references, 1);
	window_pane_remove_ref(wp, __func__);
	close(pair[1]);
	close(pipefd[0]);
	window_remove_ref(w, __func__);
}

TEST(retained_pane_survives_window_destruction)
{
	struct window *w = window_create(80, 24, 0, 0);
	struct window_pane *wp = window_add_pane(w, NULL, 100, 0);
	u_int id = wp->id;

	window_add_ref(w, __func__);
	window_pane_add_ref(wp, __func__);
	window_remove_ref(w, __func__);
	ASSERT(window_pane_find_by_id(id) == NULL);
	ASSERT(wp->flags & PANE_DESTROYED);
	ASSERT_EQ(wp->references, 1);
	ASSERT_EQ(wp->id, id);
	ASSERT_NOT_NULL(wp->base.grid);
	/* A destroyed pane's window is no longer a usable target. */
	window_pane_remove_ref(wp, __func__);
}

TEST(unretained_panes_free_on_window_destruction)
{
	struct window *w = window_create(80, 24, 0, 0);
	struct window_pane *a = window_add_pane(w, NULL, 100, 0);
	struct window_pane *b = window_add_pane(w, a, 100, 0);
	u_int aid = a->id, bid = b->id;
	window_add_ref(w, __func__);
	window_remove_ref(w, __func__);
	ASSERT(window_pane_find_by_id(aid) == NULL);
	ASSERT(window_pane_find_by_id(bid) == NULL);
}

int
main(void)
{
	setup();
	TEST_SUITE_BEGIN("Production pane lifetime");
	RUN_TEST(retained_pane_detaches_and_closes_resources);
	RUN_TEST(retained_pane_survives_window_destruction);
	RUN_TEST(unretained_panes_free_on_window_destruction);
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	TEST_SUITE_END();
}
