/* Link the complete host. Test modes exercise the production lifecycle API. */
#include "../tmate.h"
#include "test-harness.h"
#include <locale.h>

static struct cmdq_item *seen_item;
static struct cmd_find_state *seen_fs;
static struct args *seen_args;
static int initialized, freed, resized;

static struct screen *
mode_init(struct window_mode_entry *wme, struct cmdq_item *item,
    struct cmd_find_state *fs, struct args *args)
{
	struct screen *s = xmalloc(sizeof *s);
	seen_item = item;
	seen_fs = fs;
	seen_args = args;
	initialized++;
	screen_init(s, wme->wp->sx, wme->wp->sy, 0);
	return (s);
}

static struct screen *
decline_init(__unused struct window_mode_entry *wme,
    __unused struct cmdq_item *item, __unused struct cmd_find_state *fs,
    __unused struct args *args)
{
	return (NULL);
}

static void
mode_free(struct window_mode_entry *wme)
{
	freed++;
	screen_free(wme->screen);
	free(wme->screen);
}

static void
mode_resize(struct window_mode_entry *wme, u_int sx, u_int sy)
{
	resized++;
	screen_resize(wme->screen, sx, sy, 0);
}

static const struct window_mode first = {
	.name = "test-first", .init = mode_init, .free = mode_free,
	.resize = mode_resize
};
static const struct window_mode second = {
	.name = "test-second", .init = mode_init, .free = mode_free,
	.resize = mode_resize
};
static const struct window_mode transient = {
	.name = "test-transient", .flags = WINDOW_MODE_NO_STACK,
	.init = mode_init, .free = mode_free
};
static const struct window_mode declined = {
	.name = "test-declined", .init = decline_init
};

static struct window_pane *
create_pane(void)
{
	struct window *w = window_create(80, 24, 0, 0);
	struct window_pane *wp = window_add_pane(w, NULL, 100, 0);
	window_add_ref(w, __func__);
	layout_init(w, wp);
	initialized = freed = resized = 0;
	return (wp);
}

static void
destroy_pane(struct window_pane *wp)
{
	window_remove_ref(wp->window, __func__);
}

static enum cmd_retval
noop_callback(__unused struct cmdq_item *item, __unused void *data)
{
	return (CMD_RETURN_NORMAL);
}

TEST(context_and_duplicate_entry)
{
	struct window_pane *wp = create_pane();
	struct cmdq_item *item = cmdq_get_callback(noop_callback, NULL);
	struct cmd_find_state fs;
	struct args *args = args_create();
	cmd_find_clear_state(&fs, 0);
	ASSERT_EQ(window_pane_set_mode(wp, wp, &first, item, &fs, args), 0);
	ASSERT(seen_item == item && seen_fs == &fs && seen_args == args);
	ASSERT(TAILQ_FIRST(&wp->modes)->swp == wp);
	ASSERT(wp->screen == TAILQ_FIRST(&wp->modes)->screen);
	ASSERT_EQ(window_pane_set_mode(wp, NULL, &first, NULL, NULL, NULL), 1);
	ASSERT_EQ(initialized, 1);
	window_pane_reset_mode_all(wp);
	ASSERT_EQ(freed, 1);
	args_free(args);
	/* Queue ownership releases the real item through the normal queue path. */
	cmdq_append(NULL, item);
	cmdq_next(NULL);
	destroy_pane(wp);
}

TEST(declined_mode_preserves_base_and_previous_mode)
{
	struct window_pane *wp = create_pane();
	struct screen *s;
	ASSERT_EQ(window_pane_set_mode(wp, NULL, &declined, NULL, NULL, NULL), 1);
	ASSERT(TAILQ_EMPTY(&wp->modes));
	ASSERT(wp->screen == &wp->base);
	window_pane_set_mode(wp, NULL, &first, NULL, NULL, NULL);
	s = wp->screen;
	ASSERT_EQ(window_pane_set_mode(wp, NULL, &declined, NULL, NULL, NULL), 1);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &first);
	ASSERT(TAILQ_NEXT(TAILQ_FIRST(&wp->modes), entry) == NULL);
	ASSERT(wp->screen == s);
	ASSERT_EQ(freed, 0);
	destroy_pane(wp);
	ASSERT_EQ(freed, 1);
}

TEST(no_stack_replacement_restores_underlying_mode_on_decline)
{
	struct window_pane *wp = create_pane();
	window_pane_set_mode(wp, NULL, &first, NULL, NULL, NULL);
	window_pane_set_mode(wp, NULL, &transient, NULL, NULL, NULL);
	ASSERT_EQ(window_pane_set_mode(wp, NULL, &transient, NULL, NULL, NULL), 1);
	ASSERT_EQ(freed, 0);
	ASSERT_EQ(window_pane_set_mode(wp, NULL, &declined, NULL, NULL, NULL), 1);
	ASSERT_EQ(freed, 1);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &first);
	ASSERT(wp->screen == TAILQ_FIRST(&wp->modes)->screen);
	window_pane_set_mode(wp, NULL, &transient, NULL, NULL, NULL);
	ASSERT_EQ(window_pane_set_mode(wp, NULL, &second, NULL, NULL, NULL), 0);
	ASSERT_EQ(freed, 2);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &second);
	ASSERT(TAILQ_NEXT(TAILQ_FIRST(&wp->modes), entry)->mode == &first);
	destroy_pane(wp);
	ASSERT_EQ(freed, 4);
}

TEST(existing_mode_reorders_without_reinitializing)
{
	struct window_pane *wp = create_pane();
	struct screen *s;
	window_pane_set_mode(wp, NULL, &first, NULL, NULL, NULL);
	s = wp->screen;
	window_pane_set_mode(wp, NULL, &second, NULL, NULL, NULL);
	window_pane_set_mode(wp, NULL, &first, NULL, NULL, NULL);
	ASSERT_EQ(initialized, 2);
	ASSERT(wp->screen == s);
	window_pane_reset_mode(wp);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &second);
	ASSERT_EQ(resized, 1);
	window_pane_reset_mode_all(wp);
	ASSERT(wp->screen == &wp->base);
	ASSERT_EQ(freed, 2);
	destroy_pane(wp);
}

TEST(real_copy_clock_and_view_modes)
{
	struct window_pane *wp = create_pane();
	struct args *args = args_create();
	ASSERT_EQ(window_pane_set_mode(wp, wp, &window_copy_mode, NULL, NULL, args), 0);
	ASSERT_EQ(window_pane_set_mode(wp, NULL, &window_clock_mode, NULL, NULL, NULL), 0);
	window_pane_reset_mode(wp);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &window_copy_mode);
	window_pane_reset_mode_all(wp);
	ASSERT_EQ(window_pane_set_mode(wp, NULL, &window_view_mode, NULL, NULL, NULL), 0);
	window_copy_add(wp, 0, "%s", "mode output");
	ASSERT(wp->screen != &wp->base);
	window_pane_reset_mode_all(wp);
	ASSERT(wp->screen == &wp->base);
	args_free(args);
	destroy_pane(wp);
}

TEST(host_exit_only_emits_for_last_copy_mode)
{
	struct window_pane *wp = create_pane();
	struct args *args = args_create();
	struct evbuffer *buffer;
	size_t size;

	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	buffer = tmate_session.encoder.buffer;
	window_pane_set_mode(wp, NULL, &window_clock_mode, NULL, NULL, NULL);
	window_pane_set_mode(wp, wp, &window_copy_mode, NULL, NULL, args);
	tmate_sync_copy_mode(&tmate_session, wp);
	ASSERT_EQ(evbuffer_get_length(buffer), 0);
	window_pane_set_mode(wp, NULL, &window_view_mode, NULL, NULL, NULL);
	window_pane_reset_mode(wp);
	/* The underlying copy mode is active: an empty state would be false. */
	ASSERT_EQ(evbuffer_get_length(buffer), 0);
	window_pane_reset_mode(wp);
	size = evbuffer_get_length(buffer);
	ASSERT(size > 0);
	window_pane_reset_mode(wp); /* unrelated clock exit */
	ASSERT_EQ(evbuffer_get_length(buffer), size);
	window_pane_reset_mode(wp); /* no mode */
	ASSERT_EQ(evbuffer_get_length(buffer), size);
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
	args_free(args);
	destroy_pane(wp);
}

static int
output_equals(struct window_pane *wp, u_int line, const char *expected)
{
	struct window_mode_entry *wme = TAILQ_FIRST(&wp->modes);
	struct screen *backing = wme->mode->get_screen(wme);
	char *text = grid_string_cells(backing->grid, 0, line,
	    screen_size_x(backing), NULL, 0, backing);
	int result = strcmp(text, expected) == 0;
	free(text);
	return (result);
}

/* stdout contains only host-origin MessagePack, consumed by the relay test. */
static int
emit_copy_output(int exit_mode)
{
	struct window_pane *first = create_pane(), *second = create_pane();
	struct evbuffer *buffer;
	unsigned char *bytes;
	size_t size;
	int result;

	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	buffer = tmate_session.encoder.buffer;
	window_pane_set_mode(first, NULL, &window_view_mode, NULL, NULL, NULL);
	window_pane_set_mode(second, NULL, &window_view_mode, NULL, NULL, NULL);
	/* Parsed output has different semantics and must not use this opcode. */
	window_copy_add(first, 1, "%s", "parsed output");
	result = evbuffer_get_length(buffer) != 0;
	window_pane_reset_mode_all(first);
	evbuffer_drain(buffer, evbuffer_get_length(buffer));
	window_pane_set_mode(first, NULL, &window_view_mode, NULL, NULL, NULL);
	window_copy_add(first, 0, "%s %d", "literal %s", 38);
	window_copy_add(second, 0, "%s", "other pane");
	window_copy_add(first, 0, "%s", "second ⚡");
	if (exit_mode) {
		size_t before;
		window_pane_reset_mode(first);
		if (!TAILQ_EMPTY(&first->modes))
			result = 1;
		before = evbuffer_get_length(buffer);
		window_pane_reset_mode(first); /* repeated reset sends nothing */
		result |= before != evbuffer_get_length(buffer);
	}
	size = evbuffer_get_length(buffer);
	bytes = evbuffer_pullup(buffer, -1);
	result |= (!exit_mode && (!output_equals(first, 0, "literal %s 38") ||
	    !output_equals(first, 1, "second ⚡"))) ||
	    !output_equals(second, 0, "other pane");
	result |= size == 0 || fwrite(bytes, 1, size, stdout) != size;
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
	destroy_pane(second);
	destroy_pane(first);
	return (result);
}

int
main(int argc, char **argv)
{
	const struct options_table_entry *oe;
	if (setlocale(LC_CTYPE, "C.UTF-8") == NULL)
		setlocale(LC_CTYPE, "en_US.UTF-8");
	tmate_session.ev_base = event_init();
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
	if (argc == 2 && (strcmp(argv[1], "--emit-copy-output") == 0 ||
	    strcmp(argv[1], "--emit-copy-exit") == 0)) {
		int result = emit_copy_output(strcmp(argv[1], "--emit-copy-exit") == 0);
		options_free(global_w_options);
		options_free(global_s_options);
		options_free(global_options);
		environ_free(global_environ);
		return (result);
	}
	TEST_SUITE_BEGIN("Production mode lifecycle");
	RUN_TEST(context_and_duplicate_entry);
	RUN_TEST(declined_mode_preserves_base_and_previous_mode);
	RUN_TEST(no_stack_replacement_restores_underlying_mode_on_decline);
	RUN_TEST(existing_mode_reorders_without_reinitializing);
	RUN_TEST(real_copy_clock_and_view_modes);
	RUN_TEST(host_exit_only_emits_for_last_copy_mode);
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	TEST_SUITE_END();
}
