/* Complete relay production objects: real streaming decoder and mode lifecycle. */
#include "../server/tmate.h"
#include "../server/tmate-protocol.h"
#include "test-harness.h"

static int dispatched;

static void
read_message(void *session, struct tmate_unpacker *uk)
{
	tmate_dispatch_daemon_message(session, uk);
	dispatched++;
}

static void
write_output(int id, const char *text, int fragmented)
{
	struct tmate_decoder decoder;
	msgpack_sbuffer sb;
	struct tmate_encoder encoder = { .mpac_version = fragmented ? 4 : 5 };
	msgpack_packer *pk = &encoder.pk;
	char *buf;
	size_t capacity, offset = 0, count;

	msgpack_sbuffer_init(&sb);
	msgpack_packer_init(pk, &sb, msgpack_sbuffer_write);
	msgpack_pack_array(pk, 3);
	msgpack_pack_int(pk, TMATE_OUT_WRITE_COPY_MODE);
	msgpack_pack_int(pk, id);
	msgpack_pack_string(pk, text);
	tmate_decoder_init(&decoder, read_message, NULL);
	while (offset < sb.size) {
		tmate_decoder_get_buffer(&decoder, &buf, &capacity);
		count = fragmented ? 1 : sb.size - offset;
		if (count > capacity)
			count = capacity;
		memcpy(buf, sb.data + offset, count);
		tmate_decoder_commit(&decoder, count);
		offset += count;
	}
	msgpack_unpacker_destroy(&decoder.unpacker);
	msgpack_sbuffer_destroy(&sb);
}

static struct window_pane *
create_pane(void)
{
	struct window *w = window_create(80, 24, 0, 0);
	struct window_pane *wp = window_add_pane(w, NULL, 100, 0);
	window_add_ref(w, __func__);
	layout_init(w, wp);
	return (wp);
}

static void
destroy_pane(struct window_pane *wp)
{
	window_remove_ref(wp->window, __func__);
}

static int
line_equals(struct window_pane *wp, u_int line, const char *text)
{
	struct window_mode_entry *wme = TAILQ_FIRST(&wp->modes);
	struct screen *backing = wme->mode->get_screen(wme);
	char *actual = grid_string_cells(backing->grid, 0, line,
	    screen_size_x(backing), NULL, 0, backing);
	int equal = strcmp(actual, text) == 0;
	free(actual);
	return (equal);
}

TEST(initial_and_repeated_output)
{
	struct window_pane *wp = create_pane();
	struct window_mode_entry *wme;
	write_output(wp->id, "literal %s output", 0);
	wme = TAILQ_FIRST(&wp->modes);
	ASSERT(wme != NULL && wme->mode == &window_view_mode);
	ASSERT(line_equals(wp, 0, "literal %s output"));
	write_output(wp->id, "second", 0);
	ASSERT(TAILQ_FIRST(&wp->modes) == wme);
	ASSERT(TAILQ_NEXT(wme, entry) == NULL);
	ASSERT(line_equals(wp, 0, "literal %s output"));
	ASSERT(line_equals(wp, 1, "second"));
	ASSERT(wp->base.cy == 0);
	destroy_pane(wp);
}

TEST(fragmented_v4_message)
{
	struct window_pane *wp = create_pane();
	int before = dispatched;
	write_output(wp->id, "fragmented", 1);
	ASSERT_EQ(dispatched, before + 1);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &window_view_mode);
	ASSERT(line_equals(wp, 0, "fragmented"));
	destroy_pane(wp);
}

TEST(unknown_pane_is_discarded)
{
	struct window_pane *wp = create_pane();
	write_output(INT_MAX, "discarded", 0);
	ASSERT(TAILQ_EMPTY(&wp->modes));
	ASSERT(wp->screen == &wp->base);
	destroy_pane(wp);
}

TEST(active_clock_mode_is_preserved_under_output)
{
	struct window_pane *wp = create_pane();
	struct window_mode_entry *clock;
	window_pane_set_mode(wp, NULL, &window_clock_mode, NULL, NULL, NULL);
	clock = TAILQ_FIRST(&wp->modes);
	write_output(wp->id, "clock output", 0);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &window_view_mode);
	ASSERT(TAILQ_NEXT(TAILQ_FIRST(&wp->modes), entry) == clock);
	ASSERT(line_equals(wp, 0, "clock output"));
	window_pane_reset_mode(wp);
	ASSERT(TAILQ_FIRST(&wp->modes) == clock);
	ASSERT(wp->screen == clock->screen);
	destroy_pane(wp);
}

TEST(active_copy_mode_has_separate_backing)
{
	struct window_pane *wp = create_pane();
	struct args *args = args_create();
	struct window_mode_entry *copy;
	window_pane_set_mode(wp, wp, &window_copy_mode, NULL, NULL, args);
	copy = TAILQ_FIRST(&wp->modes);
	write_output(wp->id, "separate output", 0);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &window_view_mode);
	ASSERT(TAILQ_NEXT(TAILQ_FIRST(&wp->modes), entry) == copy);
	ASSERT(line_equals(wp, 0, "separate output"));
	ASSERT(copy->mode->get_screen(copy) !=
	    TAILQ_FIRST(&wp->modes)->mode->get_screen(TAILQ_FIRST(&wp->modes)));
	window_pane_reset_mode(wp);
	ASSERT(TAILQ_FIRST(&wp->modes) == copy);
	args_free(args);
	destroy_pane(wp);
}

TEST(existing_view_mode_is_resumed)
{
	struct window_pane *wp = create_pane();
	struct window_mode_entry *view, *clock;
	write_output(wp->id, "before", 0);
	view = TAILQ_FIRST(&wp->modes);
	window_pane_set_mode(wp, NULL, &window_clock_mode, NULL, NULL, NULL);
	clock = TAILQ_FIRST(&wp->modes);
	write_output(wp->id, "after", 0);
	ASSERT(TAILQ_FIRST(&wp->modes) == view);
	ASSERT(TAILQ_NEXT(view, entry) == clock);
	ASSERT(TAILQ_NEXT(clock, entry) == NULL);
	ASSERT(line_equals(wp, 0, "before"));
	ASSERT(line_equals(wp, 1, "after"));
	destroy_pane(wp);
}

int
main(void)
{
	const struct options_table_entry *oe;
	event_init();
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
	TEST_SUITE_BEGIN("Production relay copy output");
	RUN_TEST(initial_and_repeated_output);
	RUN_TEST(fragmented_v4_message);
	RUN_TEST(unknown_pane_is_discarded);
	RUN_TEST(active_clock_mode_is_preserved_under_output);
	RUN_TEST(active_copy_mode_has_separate_backing);
	RUN_TEST(existing_view_mode_is_resumed);
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	TEST_SUITE_END();
}
