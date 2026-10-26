/* Link the complete host. Test modes exercise the production lifecycle API. */
#include "../tmate.h"
#include "../tmate-protocol.h"
#include "../tmate-key.h"
#include "../tmate-layout.h"
#include "test-harness.h"
#include <locale.h>
#include "snapshot-parser-fixture.h"
#include "snapshot-pane-fixture.h"
#include "test-snapshot-parser.h"

/* Frozen protocol 6 values from the pre-port header, not internal enum math. */
TEST(sharing_key_numbers_stay_compatible)
{
	ASSERT_EQ(tmate_key_to_wire(KEYC_BSPACE, 6), 0x10e25a);
	ASSERT_EQ(tmate_key_to_wire(KEYC_F1, 6), 0x10e25b);
	ASSERT_EQ(tmate_key_to_wire(KEYC_UP, 6), 0x10e26e);
	ASSERT_EQ(tmate_key_to_wire(KEYC_RIGHT, 6), 0x10e271);
	ASSERT_EQ(tmate_key_to_wire(KEYC_FOCUS_IN, 6), 0x10e000);
	ASSERT_EQ(tmate_key_to_wire(KEYC_PASTE_END, 6), 0x10e004);
	ASSERT_EQ(tmate_key_to_wire(KEYC_BSPACE, 5), 0x10e251);
	ASSERT_EQ(tmate_key_from_wire(0x10e26e | KEYC_CTRL), KEYC_UP | KEYC_CTRL);
	ASSERT_EQ(tmate_key_from_wire(0x10f003), KEYC_USER + 3);
	ASSERT_EQ(tmate_key_from_wire(0x1f98a), 0x1f98a);
	ASSERT_EQ(tmate_key_to_wire(0x1f98a | KEYC_META, 6), 0x1f98a | KEYC_META);
	ASSERT_EQ(tmate_key_from_wire(0x10e010), KEYC_UNKNOWN);
	ASSERT_EQ(tmate_key_to_wire(KEYC_MOUSE, 6), TMATE_WIRE_UNKNOWN);
}

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

static void
copy_boundary_command(struct window_pane *wp, struct session *s,
    struct winlink *wl, const char *command)
{
	struct args_parse parse = { "", 1, 1, NULL };
	struct args_value values[2] = {
		{ .type = ARGS_STRING, .string = "send-keys" },
		{ .type = ARGS_STRING, .string = (char *)command }
	};
	struct window_mode_entry *wme = TAILQ_FIRST(&wp->modes);
	struct args *args;
	char *cause = NULL;

	args = args_parse(&parse, values, 2, &cause);
	if (args == NULL)
		fatalx("copy boundary command: %s", cause);
	wme->mode->command(wme, NULL, s, wl, args, NULL);
	args_free(args);
}

TEST(full_width_copy_commands_produce_valid_cursor_and_selection)
{
	struct window_pane *wp = create_pane();
	struct session *s = session_create(NULL, "copy-boundary", "/",
	    environ_create(), options_create(global_s_options), NULL);
	struct winlink *wl;
	struct args *args = args_create();
	struct window_copy_state state;
	struct grid_cell gc = grid_default_cell;
	u_int x;
	const char *motions[] = { "previous-word", "next-word", "previous-space",
	    "next-space", "cursor-left", "cursor-right", "start-of-line" };

	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	wl = session_attach(s, wp->window, 0, NULL);
	session_set_current(s, wl);
	options_set_number(wp->window->options, "mode-keys", MODEKEY_EMACS);
	for (x = 0; x < wp->sx; x++) {
		utf8_set(&gc.data, x % 8 == 7 ? ' ' : 'a');
		/* The final cell must be nonblank to make the line full width. */
		if (x == wp->sx - 1)
			utf8_set(&gc.data, 'z');
		grid_set_cell(wp->base.grid, x, 0, &gc);
	}
	window_pane_set_mode(wp, wp, &window_copy_mode, NULL, NULL, args);
	copy_boundary_command(wp, s, wl, "end-of-line");
	ASSERT(window_copy_get_state(wp, &state));
	ASSERT_EQ(state.cx, wp->sx);
	ASSERT(window_copy_state_is_valid(wp, &state));
	copy_boundary_command(wp, s, wl, "begin-selection");
	ASSERT(window_copy_get_state(wp, &state));
	ASSERT_EQ(state.selection, 1);
	ASSERT_EQ(state.selx, wp->sx);
	ASSERT(window_copy_state_is_valid(wp, &state));
	for (x = 0; x < nitems(motions); x++) {
		copy_boundary_command(wp, s, wl, "end-of-line");
		copy_boundary_command(wp, s, wl, motions[x]);
		ASSERT(window_copy_get_state(wp, &state));
		ASSERT(window_copy_state_is_valid(wp, &state));
	}
	state.cx = wp->sx + 1;
	ASSERT(!window_copy_state_is_valid(wp, &state));
	state.cx = wp->sx;
	state.selx = wp->sx + 1;
	ASSERT(!window_copy_state_is_valid(wp, &state));
	session_destroy(s, 0, __func__);
	destroy_pane(wp);
	event_loop(EVLOOP_NONBLOCK);
	args_free(args);
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
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
	/* Entering copy mode emits the active state. */
	ASSERT(evbuffer_get_length(buffer) > 0);
	evbuffer_drain(buffer, evbuffer_get_length(buffer));
	tmate_sync_copy_mode(&tmate_session, wp);
	ASSERT(evbuffer_get_length(buffer) > 0);
	evbuffer_drain(buffer, evbuffer_get_length(buffer));
	window_pane_set_mode(wp, NULL, &window_view_mode, NULL, NULL, NULL);
	window_pane_reset_mode(wp);
	/* Both transitions preserve the active copy state. */
	ASSERT(evbuffer_get_length(buffer) > 0);
	evbuffer_drain(buffer, evbuffer_get_length(buffer));
	window_pane_reset_mode(wp);
	size = evbuffer_get_length(buffer);
	ASSERT(size > 0); /* copy exit emits empty state while clock remains */
	window_pane_reset_mode(wp);
	ASSERT_EQ(evbuffer_get_length(buffer), size);
	window_pane_reset_mode(wp); /* no mode */
	ASSERT_EQ(evbuffer_get_length(buffer), size);
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
	args_free(args);
	destroy_pane(wp);
}

struct reconnect_messages {
	int count;
	int opcode[16];
	int argc[16];
};

static void
read_reconnect_message(void *userdata, struct tmate_unpacker *uk)
{
	struct reconnect_messages *messages = userdata;
	int n = messages->count++;

	if (n < 16) {
		messages->argc[n] = uk->argc;
		messages->opcode[n] = unpack_int(uk);
	}
}

static void
decode_reconnect_buffer(struct tmate_decoder *decoder, int fragmented)
{
	struct evbuffer *buffer = tmate_session.encoder.buffer;
	char *bytes;
	size_t capacity, count;

	while (evbuffer_get_length(buffer) != 0) {
		tmate_decoder_get_buffer(decoder, &bytes, &capacity);
		count = fragmented ? 1 : evbuffer_get_length(buffer);
		if (count > capacity)
			count = capacity;
		evbuffer_remove(buffer, bytes, count);
		tmate_decoder_commit(decoder, count);
	}
}

static void
check_empty_session_reconnect(int fragmented)
{
	struct reconnect_messages messages = { 0 };
	struct tmate_decoder decoder;
	struct window_pane *wp = create_pane();
	int before;

	ASSERT(RB_EMPTY(&sessions));
	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	/* Reconnect must discard the old stream and start with a fresh header. */
	tmate_write_copy_mode(&tmate_session, wp, "discard before reconnect");
	tmate_send_reconnection_state(&tmate_session);
	tmate_decoder_init(&decoder, read_reconnect_message, &messages);
	decode_reconnect_buffer(&decoder, fragmented);
	before = messages.count;
	/* An incomplete snapshot would swallow this as its second element. */
	tmate_write_copy_mode(&tmate_session, wp, "after reconnect");
	decode_reconnect_buffer(&decoder, fragmented);
	msgpack_unpacker_destroy(&decoder.unpacker);
	tmate_encoder_destroy(&tmate_session.encoder);
	destroy_pane(wp);

	ASSERT_EQ(before, 3);
	ASSERT_EQ(messages.count, 4);
	ASSERT_EQ(messages.opcode[0], TMATE_OUT_HEADER);
	ASSERT_EQ(messages.argc[0], 3);
	ASSERT_EQ(messages.opcode[1], TMATE_OUT_UNAME);
	ASSERT_EQ(messages.argc[1], 6);
	ASSERT_EQ(messages.opcode[2], TMATE_OUT_READY);
	ASSERT_EQ(messages.argc[2], 1);
	ASSERT_EQ(messages.opcode[3], TMATE_OUT_WRITE_COPY_MODE);
	ASSERT_EQ(messages.argc[3], 3);
}

TEST(empty_session_reconnect_keeps_message_boundaries)
{
	check_empty_session_reconnect(0);
}

TEST(fragmented_empty_session_reconnect_keeps_message_boundaries)
{
	check_empty_session_reconnect(1);
}

/* Inspect actual reconnect bytes, including both base and saved grids. */
struct snapshot_cells {
	int snapshots, layouts, after, id, mode, empty_names;
	int cx[2], cy[2], lines[2], cells[2];
	uint64_t attrs[2][2];
	char text[2][32];
	size_t length[2];
};

static void
read_snapshot_cells(void *userdata, struct tmate_unpacker *uk)
{
	struct snapshot_cells *state = userdata;
	struct tmate_unpacker panes, pane, screen, rows, row, attrs;
	const char *bytes;
	size_t length;
	int opcode = unpack_int(uk), i, j;

	if (opcode == TMATE_OUT_SYNC_LAYOUT) {
		char *name;
		state->layouts++;
		unpack_int(uk);
		unpack_int(uk);
		unpack_array(uk, &panes);
		unpack_array(&panes, &pane);
		unpack_int(&pane);
		name = unpack_string(&pane);
		state->empty_names += *name == '\0';
		free(name);
		return;
	}
	if (opcode == TMATE_OUT_WRITE_COPY_MODE)
		state->after++;
	if (opcode != TMATE_OUT_SNAPSHOT)
		return;
	state->snapshots++;
	unpack_array(uk, &panes);
	unpack_array(&panes, &pane);
	state->id = unpack_int(&pane);
	state->mode = unpack_int(&pane);
	for (i = 0; i < 2; i++) {
		unpack_array(&pane, &screen);
		state->cx[i] = unpack_int(&screen);
		state->cy[i] = unpack_int(&screen);
		unpack_array(&screen, &rows);
		state->lines[i] = rows.argc;
		unpack_array(&rows, &row);
		unpack_buffer(&row, &bytes, &length);
		state->length[i] = length;
		if (length <= sizeof state->text[i])
			memcpy(state->text[i], bytes, length);
		unpack_array(&row, &attrs);
		state->cells[i] = attrs.argc;
		for (j = 0; j < 2 && attrs.argc != 0; j++)
			state->attrs[i][j] = unpack_int(&attrs);
	}
}

static void
check_snapshot_cell_fields(int fragmented, int empty_name)
{
	struct snapshot_cells state = { 0 };
	struct tmate_decoder decoder;
	struct window_pane *wp = create_pane();
	struct session *s;
	struct winlink *wl;
	struct grid_cell gc = grid_default_cell;
	u_int id = wp->id, mode = wp->base.mode;

	s = session_create(NULL, "snapshot-fields", "/", environ_create(),
	    options_create(global_s_options), NULL);
	wl = winlink_add(&s->windows, 0);
	wl->session = s;
	winlink_set_window(wl, wp->window);
	s->curw = wl;
	window_set_active_pane(wp->window, wp, 0);
	window_set_name(wp->window, empty_name ? "" : "snapshot-fields", 0);
	/* TAB sets the high flag bit; modern attributes exceed one wire byte. */
	grid_set_tab(&gc, 1);
	gc.attr = GRID_ATTR_OVERLINE | GRID_ATTR_STRIKETHROUGH | GRID_ATTR_BRIGHT;
	gc.fg = COLOUR_FLAG_RGB | 0x123456;
	gc.bg = COLOUR_FLAG_256 | 0xab;
	grid_set_cell(wp->base.grid, 0, 0, &gc);
	gc = grid_default_cell;
	utf8_set(&gc.data, 'X');
	gc.attr = GRID_ATTR_UNDERSCORE_5 | GRID_ATTR_ITALICS;
	grid_set_cell(wp->base.grid, 1, 0, &gc);
	wp->base.cx = 7;
	wp->base.cy = 3;
	wp->base.saved_grid = grid_create(wp->sx, wp->sy, 100);
	gc = grid_default_cell;
	utf8_open(&gc.data, 0xe7);
	utf8_append(&gc.data, 0x95);
	utf8_append(&gc.data, 0x8c); /* wide UTF-8 character 界 */
	gc.attr = GRID_ATTR_STRIKETHROUGH | GRID_ATTR_REVERSE;
	grid_set_cell(wp->base.saved_grid, 0, 0, &gc);
	grid_set_padding(wp->base.saved_grid, 1, 0, 8);
	wp->base.saved_cx = 5;
	wp->base.saved_cy = 2;
	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	tmate_send_reconnection_state(&tmate_session);
	tmate_write_copy_mode(&tmate_session, wp, "after snapshot");
	tmate_decoder_init(&decoder, read_snapshot_cells, &state);
	decode_reconnect_buffer(&decoder, fragmented);
	tmate_decoder_destroy(&decoder);
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
	destroy_pane(wp);

	ASSERT_EQ(state.layouts, 1);
	ASSERT_EQ(state.empty_names, empty_name);
	ASSERT_EQ(state.snapshots, 1);
	ASSERT_EQ(state.after, 1);
	ASSERT_EQ(state.id, id);
	ASSERT_EQ(state.mode, mode);
	ASSERT_EQ(state.cx[0], 7);
	ASSERT_EQ(state.cy[0], 3);
	ASSERT_EQ(state.cx[1], 5);
	ASSERT_EQ(state.cy[1], 2);
	ASSERT_EQ(state.lines[0], 24);
	ASSERT_EQ(state.lines[1], 24);
	ASSERT(state.cells[0] >= 2);
	ASSERT(state.cells[1] >= 2);
	ASSERT_EQ(state.length[0], state.cells[0]);
	ASSERT(memcmp(state.text[0], " X", 2) == 0);
	ASSERT_EQ(state.length[1], state.cells[1] + 2);
	/* Legacy padding contributes its sentinel byte, flagged separately. */
	ASSERT(memcmp(state.text[1], "界!", 4) == 0);
	/* Fixed legacy bytes, independent of the encoder's packing expression. */
	ASSERT_EQ(state.attrs[0][0], UINT32_C(0x8001ab56));
	ASSERT_EQ(state.attrs[0][1], UINT32_C(0x00400808));
	ASSERT_EQ(state.attrs[1][0], UINT32_C(0x00100808));
	ASSERT_EQ(state.attrs[1][1], UINT32_C(0x04000808));
}

TEST(reconnect_snapshot_keeps_cell_fields_separate)
{
	check_snapshot_cell_fields(0, 0);
}

TEST(fragmented_reconnect_snapshot_keeps_cell_fields_separate)
{
	check_snapshot_cell_fields(1, 0);
}

TEST(fragmented_reconnect_empty_window_name_unpacks)
{
	check_snapshot_cell_fields(1, 1);
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
	/* State hooks are expected; remove them before the parsed-output check. */
	evbuffer_drain(buffer, evbuffer_get_length(buffer));
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

static int emit_layout, emit_frozen_copy;

static int
emit_copy_state(void)
{
	struct window_pane *wp = create_pane();
	struct args *args = args_create();
	struct window_copy_state state = {
		.view_mode = 0,
		.oy = 0,
		.cx = 5,
		.cy = 6,
		.selection = 1,
		.selx = 3,
		.transformed_sely = 0,
		.rectangle = 1
	};
	struct grid_cell gc = grid_default_cell;
	struct evbuffer *buffer;
	const unsigned char *bytes;
	size_t size;
	int result;

	utf8_set(&gc.data, 'H');
	grid_set_cell(wp->base.grid, 0, wp->base.grid->sy - 1, &gc);
	grid_scroll_history(wp->base.grid, 1);
	window_pane_set_mode(wp, wp, &window_copy_mode, NULL, NULL, args);
	window_copy_restore_state(wp, &state);
	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	window_pane_set_mode(wp, NULL, &window_clock_mode, NULL, NULL, NULL);
	buffer = tmate_session.encoder.buffer;
	tmate_sync_copy_mode(&tmate_session, wp);
	size = evbuffer_get_length(buffer);
	bytes = evbuffer_pullup(buffer, -1);
	result = size == 0 || fwrite(bytes, 1, size, stdout) != size;
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
	args_free(args);
	destroy_pane(wp);
	return (result);
}

static void
emit_snapshot_message(void *userdata, struct tmate_unpacker *uk)
{
	msgpack_packer *pk = userdata;
	int i;
	if (uk->argc == 0 || (uk->argv[0].via.i64 != TMATE_OUT_SNAPSHOT &&
	    !(emit_frozen_copy && uk->argv[0].via.i64 == TMATE_OUT_SYNC_COPY_MODE) &&
	    (!emit_layout || (uk->argv[0].via.i64 != TMATE_OUT_SYNC_LAYOUT &&
	    uk->argv[0].via.i64 != TMATE_OUT_PTY_DATA))))
		return;
	msgpack_pack_array(pk, uk->argc);
	for (i = 0; i < uk->argc; i++)
		msgpack_pack_object(pk, uk->argv[i]);
}

/* Filter the actual reconnect stream, preserving the entire snapshot object. */
static int
emit_reconnect_snapshot(void)
{
	struct window_pane *first = create_pane(), *second = create_pane();
	struct session *s;
	struct winlink *wl;
	struct grid_cell gc = grid_default_cell;
	struct tmate_decoder decoder;
	msgpack_sbuffer sb;
	msgpack_packer pk;
	int result;

	s = session_create(NULL, "restore-grids", "/", environ_create(),
	    options_create(global_s_options), NULL);
	wl = winlink_add(&s->windows, 0);
	wl->session = s;
	winlink_set_window(wl, first->window);
	s->curw = wl;
	wl = winlink_add(&s->windows, 1);
	wl->session = s;
	winlink_set_window(wl, second->window);
	if (emit_layout && !emit_frozen_copy) {
		window_resize(first->window, 67, 19, -1, -1);
		window_pane_resize(first, 67, 19);
		window_pane_resize(second, 43, 12);
		second->xoff = 7;
		second->yoff = 4;
	}
	window_set_active_pane(first->window, first, 0);
	window_set_active_pane(second->window, second, 0);
	grid_destroy(first->base.grid);
	first->base.grid = grid_create(first->sx, first->sy + 1, 100);
	first->base.grid->sy = first->sy;
	first->base.grid->hsize = 1;
	utf8_set(&gc.data, 'H');
	grid_set_cell(first->base.grid, 0, 0, &gc);
	grid_set_tab(&gc, 1);
	gc.attr = GRID_ATTR_BRIGHT;
	grid_set_cell(first->base.grid, 0, 1, &gc);
	gc = grid_default_cell;
	utf8_open(&gc.data, 0xe7);
	utf8_append(&gc.data, 0x95);
	utf8_append(&gc.data, 0x8c);
	gc.attr = GRID_ATTR_REVERSE;
	grid_set_cell(first->base.grid, 1, 1, &gc);
	grid_set_padding(first->base.grid, 2, 1, 8);
	first->base.cx = first->sx;
	first->base.cy = 3;
	first->base.saved_grid = grid_create(first->sx, first->sy, 0);
	gc = grid_default_cell;
	utf8_set(&gc.data, 'S');
	grid_set_cell(first->base.saved_grid, 0, 0, &gc);
	first->base.saved_cx = 5;
	first->base.saved_cy = 2;
	gc = grid_default_cell;
	utf8_set(&gc.data, 'B');
	grid_set_cell(second->base.grid, 0, 0, &gc);
	if (emit_frozen_copy) {
		struct args *args = args_create();
		gc = grid_default_cell;
		utf8_set(&gc.data, 'F');
		grid_set_cell(first->base.grid, 0, 1, &gc);
		first->base.grid->linedata[1].cellused = 3;
		first->base.cx = 5;
		window_pane_set_mode(first, first, &window_copy_mode, NULL, NULL, args);
		args_free(args);
		utf8_set(&gc.data, 'L');
		grid_set_cell(first->base.grid, 0, 1, &gc);
	}
	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	tmate_send_reconnection_state(&tmate_session);
	if (emit_layout && !emit_frozen_copy) {
		tmate_pty_data(&tmate_session, first, "\033[10;4HZ", 8);
		tmate_pty_data(&tmate_session, second, "\033[2;3HY", 7);
	}
	msgpack_sbuffer_init(&sb);
	msgpack_packer_init(&pk, &sb, msgpack_sbuffer_write);
	tmate_decoder_init(&decoder, emit_snapshot_message, &pk);
	decode_reconnect_buffer(&decoder, 1);
	result = sb.size == 0 || fwrite(sb.data, 1, sb.size, stdout) != sb.size;
	msgpack_sbuffer_destroy(&sb);
	tmate_decoder_destroy(&decoder);
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
	destroy_pane(second);
	destroy_pane(first);
	return (result);
}

/* Real split, resize and close APIs generate all wire geometry. Reverse the
 * spatial and pane-list orders with SPAWN_BEFORE to exercise tree ordering. */
static int
emit_split_reconnect(int vertical, int mixed)
{
	struct window_pane *first = create_pane(), *second, *third = NULL;
	struct window *w = first->window;
	struct session *s;
	struct winlink *wl;
	struct layout_cell *lc;
	struct tmate_decoder decoder;
	struct grid_cell gc = grid_default_cell;
	msgpack_sbuffer sb;
	msgpack_packer pk;
	int phase, result;

	s = session_create(NULL, "split-reconnect", "/", environ_create(),
	    options_create(global_s_options), NULL);
	wl = winlink_add(&s->windows, 0);
	wl->session = s;
	winlink_set_window(wl, w);
	s->curw = wl;
	lc = layout_split_pane(first, vertical ? LAYOUT_TOPBOTTOM :
	    LAYOUT_LEFTRIGHT, vertical ? 9 : 29, SPAWN_BEFORE);
	if (lc == NULL)
		abort();
	second = window_add_pane(w, first, 100, 0);
	layout_assign_pane(lc, second, 0);
	layout_fix_panes(w, NULL);
	if (mixed) {
		lc = layout_split_pane(first, vertical ? LAYOUT_LEFTRIGHT :
		    LAYOUT_TOPBOTTOM, vertical ? 23 : 7, SPAWN_BEFORE);
		if (lc == NULL)
			abort();
		third = window_add_pane(w, first, 100, 0);
		layout_assign_pane(lc, third, 0);
		layout_fix_panes(w, NULL);
	}
	window_set_active_pane(w, second, 0);
	emit_layout = 1;
	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	msgpack_sbuffer_init(&sb);
	msgpack_packer_init(&pk, &sb, msgpack_sbuffer_write);
	for (phase = 0; phase < 3; phase++) {
		if (phase == 1) {
			layout_resize(w, 91, 31);
			window_resize(w, 91, 31, -1, -1);
		} else if (phase == 2) {
			layout_close_pane(first);
			window_remove_pane(w, first);
			first = NULL;
		}
		if (first) {
			utf8_set(&gc.data, 'A');
			grid_set_cell(first->base.grid, 0, 0, &gc);
		}
		if (third) {
			utf8_set(&gc.data, 'C');
			grid_set_cell(third->base.grid, 0, 0, &gc);
		}
		utf8_set(&gc.data, 'B');
		grid_set_cell(second->base.grid, 0, 0, &gc);
		tmate_send_reconnection_state(&tmate_session);
		if (first)
			tmate_pty_data(&tmate_session, first, "\033[1;1HZ", 7);
		tmate_pty_data(&tmate_session, second, "\033[1;1HY", 7);
		if (third)
			tmate_pty_data(&tmate_session, third, "\033[1;1HW", 7);
		tmate_decoder_init(&decoder, emit_snapshot_message, &pk);
		decode_reconnect_buffer(&decoder, 1);
		tmate_decoder_destroy(&decoder);
	}
	result = sb.size == 0 || fwrite(sb.data, 1, sb.size, stdout) != sb.size;
	msgpack_sbuffer_destroy(&sb);
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
	destroy_pane(second);
	return (result);
}

static void
negotiation_order(int order)
{
	struct window_pane *wp = create_pane();
	struct session *s;
	struct tmate_env *env;
	struct tmate_decoder decoder;
	struct reconnect_messages messages = { 0 };
	struct winlink *wl;

	s = session_create(NULL, "negotiation", "/", environ_create(),
	    options_create(global_s_options), NULL);
	wl = winlink_add(&s->windows, 0);
	wl->session = s;
	winlink_set_window(wl, wp->window);
	s->curw = wl;
	window_set_active_pane(wp->window, wp, 0);
	TAILQ_INIT(&tmate_session.env);
	tmate_session.layout_required = true;
	tmate_session.snapshot_required = order != 0;
	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	tmate_write_ready(&tmate_session);
	tmate_sync_layout(&tmate_session, s);
	ASSERT(tmate_session.layout_ready_pending);
	ASSERT_EQ(evbuffer_get_length(tmate_session.encoder.buffer), 0);
	if (order == 1) tmate_snapshot_negotiated(&tmate_session);
	else tmate_layout_negotiated(&tmate_session);
	if (order) {
		ASSERT_EQ(evbuffer_get_length(tmate_session.encoder.buffer), 0);
		tmate_pty_data(&tmate_session, wp, "WITHHELD", 8);
		tmate_sync_copy_mode(&tmate_session, wp);
		tmate_write_copy_mode(&tmate_session, wp, "WITHHELD");
		ASSERT_EQ(evbuffer_get_length(tmate_session.encoder.buffer), 0);
		if (order == 1) tmate_layout_negotiated(&tmate_session);
		else tmate_snapshot_negotiated(&tmate_session);
	}
	ASSERT(tmate_session.layout_capability);
	tmate_decoder_init(&decoder, read_reconnect_message, &messages);
	decode_reconnect_buffer(&decoder, 1);
	ASSERT(messages.count > 2 && messages.count < 16);
	ASSERT_EQ(messages.opcode[0], TMTV_LAYOUT_OPCODE);
	if (order) ASSERT_EQ(messages.opcode[1], TMTV_SNAPSHOT_OPCODE);
	ASSERT_EQ(messages.opcode[messages.count-1], TMATE_OUT_READY);
	tmate_layout_negotiated(&tmate_session);
	tmate_sync_layout(&tmate_session, s);
	ASSERT_EQ(evbuffer_get_length(tmate_session.encoder.buffer), 0);
	tmate_decoder_destroy(&decoder);
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
	free(tmate_session.layout_sent);
	tmate_session.layout_sent = NULL; tmate_session.layout_sent_len = 0;
	tmate_session.layout_required = tmate_session.layout_capability = false;
	tmate_session.snapshot_required = tmate_session.snapshot_capability = false;
	while ((env = TAILQ_FIRST(&tmate_session.env)) != NULL) {
		TAILQ_REMOVE(&tmate_session.env, env, entry);
		free(env->name); free(env->value); free(env);
	}
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0) ;
	event_loop(EVLOOP_NONBLOCK);
	destroy_pane(wp);
}

TEST(layout_negotiation_holds_ready_and_sends_state_once) { negotiation_order(0); }
TEST(snapshot_negotiation_waits_for_layout_after_snapshot) { negotiation_order(1); }
TEST(snapshot_negotiation_waits_for_snapshot_after_layout) { negotiation_order(2); }

TEST(layout_timeout_cannot_be_revived_by_late_capability)
{
	struct tmate_env *env;
	struct tmate_decoder decoder;
	struct reconnect_messages messages = { 0 };
	TAILQ_INIT(&tmate_session.env);
	tmate_session.layout_required = true;
	tmate_encoder_init(&tmate_session.encoder, NULL, NULL);
	tmate_write_ready(&tmate_session);
	tmate_layout_capability_timeout(-1, 0, &tmate_session);
	ASSERT(tmate_session.layout_failed && !tmate_session.layout_capability);
	tmate_layout_negotiated(&tmate_session);
	tmate_snapshot_negotiated(&tmate_session);
	tmate_write_ready(&tmate_session);
	ASSERT(!tmate_session.layout_capability && !tmate_session.snapshot_capability);
	tmate_decoder_init(&decoder, read_reconnect_message, &messages);
	decode_reconnect_buffer(&decoder, 1);
	ASSERT_EQ(messages.count, 1);
	ASSERT_EQ(messages.opcode[0], TMATE_OUT_FIN);
	tmate_decoder_destroy(&decoder);
	tmate_encoder_destroy(&tmate_session.encoder);
	tmate_session.encoder.buffer = NULL;
	tmate_session.layout_required = tmate_session.layout_failed = false;
	tmate_session.layout_ready_pending = false;
	while ((env = TAILQ_FIRST(&tmate_session.env)) != NULL) {
		TAILQ_REMOVE(&tmate_session.env, env, entry);
		free(env->name); free(env->value); free(env);
	}
}

TEST(host_pane_snapshot_preserves_alternate_state_and_exit)
{
	struct screen source;
	struct window_pane *wp = create_pane();
	struct tmtv_snapshot_pane *p;
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	pane_fixture_screen(&source); msgpack_sbuffer_init(&sb);
	ASSERT(pane_fixture_expected(&source));
	ASSERT_EQ(tmtv_snapshot_pane_encode(wp->id, &source, 100, &sb), 0);
	msgpack_unpacked_init(&un);
	ASSERT_EQ(msgpack_unpack_next(&un, sb.data, sb.size, NULL), MSGPACK_UNPACK_SUCCESS);
	p = tmtv_snapshot_pane_decode(un.data); ASSERT_NOT_NULL(p);
	ASSERT_EQ(tmtv_snapshot_pane_apply(wp, p), 0);
	ASSERT(pane_fixture_expected(&wp->base)); ASSERT(pane_fixture_exit(&wp->base));
	tmtv_snapshot_pane_free(p); screen_free(&source); destroy_pane(wp);
	msgpack_sbuffer_destroy(&sb); msgpack_unpacked_destroy(&un);
}

TEST(host_parser_snapshot_preserves_colours_attributes_and_unicode)
{
	msgpack_sbuffer sb;
	msgpack_unpacked un;
	struct grid *gd;
	msgpack_sbuffer_init(&sb);
	ASSERT_EQ(snapshot_parser_frame(&sb), 0);
	msgpack_unpacked_init(&un);
	ASSERT_EQ(msgpack_unpack_next(&un, sb.data, sb.size, NULL), MSGPACK_UNPACK_SUCCESS);
	gd = tmtv_snapshot_grid_decode(un.data, 2000);
	ASSERT(snapshot_parser_expected(gd));
	grid_destroy(gd); msgpack_unpacked_destroy(&un); msgpack_sbuffer_destroy(&sb);
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
	if (argc == 2 && strcmp(argv[1], "--copy-boundary") == 0) {
		TEST_SUITE_BEGIN("Production copy boundary");
		RUN_TEST(full_width_copy_commands_produce_valid_cursor_and_selection);
		TEST_SUITE_END();
	}
	if (argc == 2 && strcmp(argv[1], "--emit-pane-snapshot") == 0) {
		struct screen source;
		msgpack_sbuffer sb;
		int result;
		pane_fixture_screen(&source); msgpack_sbuffer_init(&sb);
		result = tmtv_snapshot_pane_encode(0, &source, 100, &sb);
		if (result == 0 && fwrite(sb.data, 1, sb.size, stdout) != sb.size) result = -1;
		screen_free(&source); msgpack_sbuffer_destroy(&sb);
		return (result != 0);
	}
	if (argc == 2 && strcmp(argv[1], "--emit-grid-snapshot") == 0) {
		msgpack_sbuffer sb;
		int result;
		msgpack_sbuffer_init(&sb);
		result = snapshot_parser_frame(&sb);
		if (result == 0 && fwrite(sb.data, 1, sb.size, stdout) != sb.size) result = -1;
		msgpack_sbuffer_destroy(&sb);
		return (result != 0);
	}
	if (argc == 2 && (strcmp(argv[1], "--emit-copy-output") == 0 ||
	    strcmp(argv[1], "--emit-copy-exit") == 0 ||
	    strcmp(argv[1], "--emit-copy-state") == 0 ||
	    strcmp(argv[1], "--emit-frozen-copy") == 0 ||
	    strcmp(argv[1], "--emit-snapshot") == 0 ||
	    strcmp(argv[1], "--emit-layout-snapshot") == 0 ||
	    strcmp(argv[1], "--emit-vertical-reconnect") == 0 ||
	    strcmp(argv[1], "--emit-horizontal-reconnect") == 0 ||
	    strcmp(argv[1], "--emit-mixed-vertical-reconnect") == 0 ||
	    strcmp(argv[1], "--emit-mixed-horizontal-reconnect") == 0)) {
		emit_frozen_copy = strcmp(argv[1], "--emit-frozen-copy") == 0;
		emit_layout = emit_frozen_copy || strcmp(argv[1], "--emit-layout-snapshot") == 0;
		int result = strcmp(argv[1], "--emit-copy-state") == 0 ?
		    emit_copy_state() :
		    strstr(argv[1], "-reconnect") != NULL ?
		    emit_split_reconnect(strstr(argv[1], "vertical") != NULL,
		    strstr(argv[1], "mixed") != NULL) :
		    (emit_layout || strcmp(argv[1], "--emit-snapshot") == 0) ?
		    emit_reconnect_snapshot() :
		    emit_copy_output(strcmp(argv[1], "--emit-copy-exit") == 0);
		options_free(global_w_options);
		options_free(global_s_options);
		options_free(global_options);
		environ_free(global_environ);
		return (result);
	}
	TEST_SUITE_BEGIN("Production mode lifecycle");
	RUN_TEST(parser_snapshot_preserves_split_sgr_and_saved_cursor);
	RUN_TEST(parser_snapshot_preserves_partial_unicode_and_repeat);
		RUN_TEST(parser_snapshot_preserves_full_32_byte_cluster_and_continuation);
	RUN_TEST(parser_snapshot_preserves_character_sets);
	RUN_TEST(parser_snapshot_preserves_osc_and_c0_without_replaying);
	RUN_TEST(parser_snapshot_preserves_partial_dcs);
	RUN_TEST(parser_snapshot_preserves_hyperlink_and_saved_link_at_every_split);
	RUN_TEST(parser_snapshot_does_not_read_inert_completed_large_osc);
	RUN_TEST(parser_snapshot_absent_palette_clears_stale_overrides);
	RUN_TEST(host_pane_snapshot_preserves_alternate_state_and_exit);
	RUN_TEST(host_parser_snapshot_preserves_colours_attributes_and_unicode);
	RUN_TEST(sharing_key_numbers_stay_compatible);
	RUN_TEST(layout_negotiation_holds_ready_and_sends_state_once);
	RUN_TEST(snapshot_negotiation_waits_for_layout_after_snapshot);
	RUN_TEST(snapshot_negotiation_waits_for_snapshot_after_layout);
	RUN_TEST(layout_timeout_cannot_be_revived_by_late_capability);
	RUN_TEST(context_and_duplicate_entry);
	RUN_TEST(declined_mode_preserves_base_and_previous_mode);
	RUN_TEST(no_stack_replacement_restores_underlying_mode_on_decline);
	RUN_TEST(existing_mode_reorders_without_reinitializing);
	RUN_TEST(real_copy_clock_and_view_modes);
	RUN_TEST(full_width_copy_commands_produce_valid_cursor_and_selection);
	RUN_TEST(host_exit_only_emits_for_last_copy_mode);
	RUN_TEST(empty_session_reconnect_keeps_message_boundaries);
	RUN_TEST(fragmented_empty_session_reconnect_keeps_message_boundaries);
	RUN_TEST(reconnect_snapshot_keeps_cell_fields_separate);
	RUN_TEST(fragmented_reconnect_snapshot_keeps_cell_fields_separate);
	RUN_TEST(fragmented_reconnect_empty_window_name_unpacks);
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	TEST_SUITE_END();
}
