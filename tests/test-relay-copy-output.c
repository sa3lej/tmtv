/* Complete relay production objects: real streaming decoder and mode lifecycle. */
#include "../server/tmate.h"
#include "../server/tmate-protocol.h"
#include "../tmate-layout.h"
#include "test-harness.h"
#include <locale.h>

static int dispatched;

static void
read_message(void *session, struct tmate_unpacker *uk)
{
	tmate_dispatch_daemon_message(session, uk);
	dispatched++;
}

static void
write_output(int64_t id, const char *text, int fragmented)
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
	msgpack_pack_int64(pk, id);
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

static void
sync_state(int64_t id, int nonempty)
{
	struct tmate_decoder decoder;
	msgpack_sbuffer sb;
	msgpack_packer pk;
	char *buf;
	size_t capacity;

	msgpack_sbuffer_init(&sb);
	msgpack_packer_init(&pk, &sb, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, TMATE_OUT_SYNC_COPY_MODE);
	msgpack_pack_int64(&pk, id);
	msgpack_pack_array(&pk, nonempty ? 1 : 0);
	if (nonempty)
		msgpack_pack_int(&pk, 0);
	tmate_decoder_init(&decoder, read_message, NULL);
	tmate_decoder_get_buffer(&decoder, &buf, &capacity);
	if (capacity < sb.size)
		abort();
	memcpy(buf, sb.data, sb.size);
	tmate_decoder_commit(&decoder, sb.size);
	msgpack_unpacker_destroy(&decoder.unpacker);
	msgpack_sbuffer_destroy(&sb);
}

static void
decode_buffer(struct tmate_decoder *decoder, msgpack_sbuffer *sb, int fragmented)
{
	char *buf;
	size_t capacity, count, offset = 0;

	while (offset < sb->size) {
		tmate_decoder_get_buffer(decoder, &buf, &capacity);
		count = fragmented ? 1 : sb->size - offset;
		if (count > capacity)
			count = capacity;
		memcpy(buf, sb->data + offset, count);
		tmate_decoder_commit(decoder, count);
		offset += count;
	}
}

static void
pack_copy_state(msgpack_sbuffer *sb, int view_mode, int64_t id, int64_t oy, int64_t cx,
    int64_t cy, int selection, int64_t selx, int64_t transformed_sely,
    int64_t rectangle)
{
	msgpack_packer pk;

	msgpack_packer_init(&pk, sb, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, TMATE_OUT_SYNC_COPY_MODE);
	msgpack_pack_int64(&pk, id);
	msgpack_pack_array(&pk, 6);
	msgpack_pack_int(&pk, !view_mode);
	msgpack_pack_int64(&pk, oy);
	msgpack_pack_int64(&pk, cx);
	msgpack_pack_int64(&pk, cy);
	msgpack_pack_array(&pk, selection ? 3 : 0);
	if (selection) {
		msgpack_pack_int64(&pk, selx);
		msgpack_pack_int64(&pk, transformed_sely);
		msgpack_pack_int64(&pk, rectangle);
	}
	msgpack_pack_array(&pk, 0);
}

static void
pack_output(msgpack_sbuffer *sb, int64_t id, const char *text)
{
	msgpack_packer pk;

	msgpack_packer_init(&pk, sb, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, TMATE_OUT_WRITE_COPY_MODE);
	msgpack_pack_int64(&pk, id);
	/* This is a standalone packer, not the production encoder container. */
	msgpack_pack_str(&pk, strlen(text));
	msgpack_pack_str_body(&pk, text, strlen(text));
}

static void
decode_copy_state(int64_t id, int64_t oy, int64_t cx, int64_t cy, int selection,
    int64_t selx, int64_t transformed_sely, int64_t rectangle, int fragmented)
{
	struct tmate_decoder decoder;
	msgpack_sbuffer sb;

	msgpack_sbuffer_init(&sb);
	pack_copy_state(&sb, 0, id, oy, cx, cy, selection, selx,
	    transformed_sely, rectangle);
	tmate_decoder_init(&decoder, read_message, NULL);
	decode_buffer(&decoder, &sb, fragmented);
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

TEST(relay_paste_preserves_utf8_bytes_and_filters_brackets)
{
	struct window_pane *wp = create_pane();
	struct tmate_session session = { 0 }, *previous = tmate_session;
	msgpack_unpacked un;
	struct evbuffer *output;
	const char text[] = "å界🦊";
	char *bytes;
	size_t i, offset = 0, length;

	tmate_session = &session;
	session.client_protocol_version = TMATE_PROTOCOL_VERSION;
	tmate_encoder_init(&session.daemon_encoder, NULL, NULL);
	output = session.daemon_encoder.buffer;
	window_pane_paste(wp, KEYC_PASTE_START, "\033[200~", 6);
	ASSERT_EQ(evbuffer_get_length(output), 0);
	window_pane_paste(wp, 0, (char *)text, sizeof text - 1);
	length = evbuffer_get_length(output);
	bytes = (char *)evbuffer_pullup(output, -1);
	msgpack_unpacked_init(&un);
	for (i = 0; i < sizeof text - 1; i++) {
		ASSERT_EQ(msgpack_unpack_next(&un, bytes, length, &offset), MSGPACK_UNPACK_SUCCESS);
		ASSERT_EQ(un.data.type, MSGPACK_OBJECT_ARRAY);
		ASSERT_EQ(un.data.via.array.size, 3);
		ASSERT_EQ(un.data.via.array.ptr[0].via.u64, TMATE_IN_PANE_KEY);
		ASSERT_EQ(un.data.via.array.ptr[1].via.u64, wp->id);
		ASSERT_EQ(un.data.via.array.ptr[2].via.u64, KEYC_LITERAL | (u_char)text[i]);
	}
	ASSERT_EQ(offset, length);
	msgpack_unpacked_destroy(&un);
	evbuffer_drain(output, length);
	wp->screen->mode |= MODE_BRACKETPASTE;
	window_pane_paste(wp, KEYC_PASTE_START, "\033[200~", 6);
	ASSERT(evbuffer_get_length(output) > 0);
	event_free(session.daemon_encoder.ev_buffer);
	evbuffer_free(output);
	tmate_session = previous;
	destroy_pane(wp);
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

TEST(invalid_output_id_cannot_enter_mode)
{
	struct window_pane *wp = create_pane();
	int before = dispatched;

	write_output((int64_t)UINT_MAX + 1 + wp->id, "aliased", 1);
	write_output((int64_t)wp->id - ((int64_t)UINT_MAX + 1), "negative", 0);
	write_output(INT64_MAX, "maximum", 1);
	write_output(INT64_MIN, "minimum", 0);
	ASSERT_EQ(dispatched, before + 4);
	ASSERT(TAILQ_EMPTY(&wp->modes));
	ASSERT(wp->screen == &wp->base);
	write_output(wp->id, "valid after rejection", 1);
	ASSERT(line_equals(wp, 0, "valid after rejection"));
	destroy_pane(wp);
}

TEST(invalid_output_id_preserves_stack_and_backing)
{
	struct window_pane *wp = create_pane();
	struct window_mode_entry *view, *clock;

	write_output(wp->id, "preserved", 0);
	view = TAILQ_FIRST(&wp->modes);
	window_pane_set_mode(wp, NULL, &window_clock_mode, NULL, NULL, NULL);
	clock = TAILQ_FIRST(&wp->modes);
	write_output((int64_t)UINT_MAX + 1 + wp->id, "aliased", 0);
	write_output((int64_t)wp->id - ((int64_t)UINT_MAX + 1), "negative", 1);
	ASSERT(TAILQ_FIRST(&wp->modes) == clock);
	ASSERT(TAILQ_NEXT(clock, entry) == view);
	ASSERT(wp->screen == clock->screen);
	window_pane_reset_mode(wp);
	ASSERT(line_equals(wp, 0, "preserved"));
	ASSERT(line_equals(wp, 1, ""));
	write_output(wp->id, "valid next", 0);
	ASSERT(line_equals(wp, 1, "valid next"));
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

TEST(empty_state_exits_copy_and_preserves_unrelated_modes)
{
	struct window_pane *wp = create_pane();
	struct args *args = args_create();
	struct window_mode_entry *clock;
	window_pane_set_mode(wp, NULL, &window_clock_mode, NULL, NULL, NULL);
	clock = TAILQ_FIRST(&wp->modes);
	window_pane_set_mode(wp, wp, &window_copy_mode, NULL, NULL, args);
	sync_state(wp->id, 0);
	ASSERT(TAILQ_FIRST(&wp->modes) == clock);
	ASSERT(wp->screen == clock->screen);
	sync_state(wp->id, 0);
	ASSERT(TAILQ_FIRST(&wp->modes) == clock);
	window_pane_reset_mode(wp);
	sync_state(wp->id, 0);
	ASSERT(TAILQ_EMPTY(&wp->modes));
	ASSERT(wp->screen == &wp->base);
	write_output(wp->id, "exit to base", 1);
	sync_state(wp->id, 0);
	ASSERT(TAILQ_EMPTY(&wp->modes));
	ASSERT(wp->screen == &wp->base);
	args_free(args);
	destroy_pane(wp);
}

TEST(nonempty_and_invalid_pane_state_do_not_exit)
{
	struct window_pane *wp = create_pane();
	struct window_mode_entry *view;
	write_output(wp->id, "retained", 0);
	view = TAILQ_FIRST(&wp->modes);
	sync_state(wp->id, 1);
	sync_state(-1, 0);
	sync_state((int64_t)UINT_MAX + 1 + wp->id, 0);
	sync_state(INT_MAX, 0);
	ASSERT(TAILQ_FIRST(&wp->modes) == view);
	ASSERT(line_equals(wp, 0, "retained"));
	destroy_pane(wp);
}

TEST(valid_copy_state_restores_cursor_selection_and_stack)
{
	struct window_pane *wp = create_pane();
	struct grid_cell gc = grid_default_cell;
	struct window_copy_state state = { 0 };
	struct window_mode_entry *clock;
	int before = dispatched;

	utf8_set(&gc.data, 'H');
	grid_set_cell(wp->base.grid, 0, wp->base.grid->sy - 1, &gc);
	grid_scroll_history(wp->base.grid, 1);
	window_pane_set_mode(wp, NULL, &window_clock_mode, NULL, NULL, NULL);
	clock = TAILQ_FIRST(&wp->modes);
	decode_copy_state(wp->id, 0, 5, 6, 1, 3, 0, 1, 0);
	ASSERT_EQ(dispatched, before + 1);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &window_copy_mode);
	ASSERT(TAILQ_NEXT(TAILQ_FIRST(&wp->modes), entry) == clock);
	ASSERT(window_copy_get_state(wp, &state));
	ASSERT_EQ(state.view_mode, 0);
	ASSERT_EQ(state.oy, 0);
	ASSERT_EQ(state.cx, 5);
	ASSERT_EQ(state.cy, 6);
	ASSERT_EQ(state.selection, 1);
	ASSERT_EQ(state.selx, 3);
	ASSERT_EQ(state.transformed_sely, 0);
	ASSERT_EQ(state.rectangle, 1);
	ASSERT(clock->screen != wp->screen);
	destroy_pane(wp);
}

TEST(fragmented_valid_copy_state_restores)
{
	struct window_pane *wp = create_pane();
	int before = dispatched;

	decode_copy_state(wp->id, 0, 4, 3, 0, 0, 0, 0, 1);
	ASSERT_EQ(dispatched, before + 1);
	ASSERT(TAILQ_FIRST(&wp->modes) != NULL);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &window_copy_mode);
	destroy_pane(wp);
}

TEST(malformed_copy_state_is_atomic_and_keeps_boundaries)
{
	struct window_pane *wp = create_pane();
	struct tmate_decoder decoder;
	msgpack_sbuffer sb;
	int before = dispatched;
	static const int64_t invalid[][6] = {
		{ -1, 5, 6, 1, 3, 1 },
		{ 0, -5, 6, 1, 3, 1 },
		{ 0, 5, -6, 1, 3, 1 },
		{ 0, 5, 6, 1, -3, 1 },
		{ 0, 5, 6, 1, 3, 2 },
		{ 0, 5, 24, 0, 0, 0 },
		{ 0, 81, 6, 0, 0, 0 },
		{ 2, 5, 6, 0, 0, 0 },
		{ 0, 5, 6, 1, 81, 0 }
	};
	size_t i;

	for (i = 0; i < nitems(invalid); i++) {
		decode_copy_state(wp->id, invalid[i][0], invalid[i][1], invalid[i][2],
		    1, invalid[i][3], invalid[i][4], invalid[i][5], 1);
		ASSERT(TAILQ_EMPTY(&wp->modes));
	}
	ASSERT_EQ(dispatched, before + (int)nitems(invalid));
	msgpack_sbuffer_init(&sb);
	pack_copy_state(&sb, 0, wp->id, 0, 1, 2, 1, 3, INT64_MIN, 1);
	pack_output(&sb, wp->id, "after rejection");
	tmate_decoder_init(&decoder, read_message, NULL);
	decode_buffer(&decoder, &sb, 1);
	msgpack_unpacker_destroy(&decoder.unpacker);
	msgpack_sbuffer_destroy(&sb);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &window_view_mode);
	ASSERT(line_equals(wp, 0, "after rejection"));
	destroy_pane(wp);
}

TEST(view_state_uses_independent_backing_history)
{
	struct window_pane *wp = create_pane();
	struct window_mode_entry *wme;
	struct window_copy_state state;
	struct tmate_decoder decoder;
	msgpack_sbuffer sb;
	u_int i, hsize;

	for (i = 0; i < 40; i++)
		write_output(wp->id, "view history", 0);
	wme = TAILQ_FIRST(&wp->modes);
	hsize = screen_hsize(wme->mode->get_screen(wme));
	ASSERT(hsize > 0);
	ASSERT_EQ(screen_hsize(&wp->base), 0);
	msgpack_sbuffer_init(&sb);
	pack_copy_state(&sb, 1, wp->id, hsize, 5, 3, 1, 2,
	    hsize + wp->sy - 1 - 3, 1);
	tmate_decoder_init(&decoder, read_message, NULL);
	decode_buffer(&decoder, &sb, 1);
	msgpack_unpacker_destroy(&decoder.unpacker);
	msgpack_sbuffer_destroy(&sb);
	ASSERT(window_copy_get_state(wp, &state));
	ASSERT_EQ(state.view_mode, 1);
	ASSERT_EQ(state.oy, hsize);
	ASSERT_EQ(state.cx, 5);
	ASSERT_EQ(state.cy, 3);
	ASSERT_EQ(state.selection, 1);
	ASSERT_EQ(state.rectangle, 1);
	ASSERT_EQ(screen_hsize(&wp->base), 0);
	destroy_pane(wp);
}

/* Exercise the legacy control snapshot serializer used by snapshot requests.
 * The current browser renders VT data and has no CTL_SNAPSHOT cell decoder. */
struct web_snapshot {
	int messages, panes, id, cx, cy, mode, rows;
	int cells[25], arity_ok;
	uint64_t attrs[25][7];
	char text[25][96];
	size_t length[25];
};

static void
read_web_snapshot(void *userdata, struct tmate_unpacker *uk)
{
	struct web_snapshot *state = userdata;
	struct tmate_unpacker panes, pane, cursor, rows, row, attrs;
	const char *bytes;
	size_t length;
	int i, j;

	state->messages++;
	state->arity_ok = uk->argc == 2 && unpack_int(uk) == TMATE_CTL_SNAPSHOT;
	unpack_array(uk, &panes);
	state->panes = panes.argc;
	if (panes.argc == 0)
		return;
	unpack_array(&panes, &pane);
	state->arity_ok &= pane.argc == 4;
	state->id = unpack_int(&pane);
	unpack_array(&pane, &cursor);
	state->arity_ok &= cursor.argc == 2;
	state->cx = unpack_int(&cursor);
	state->cy = unpack_int(&cursor);
	state->mode = unpack_int(&pane);
	unpack_array(&pane, &rows);
	state->rows = rows.argc;
	for (i = 0; rows.argc && i < 25; i++) {
		unpack_array(&rows, &row);
		state->arity_ok &= row.argc == 2;
		unpack_buffer(&row, &bytes, &length);
		state->length[i] = length;
		if (length != 0 && length <= sizeof state->text[i])
			memcpy(state->text[i], bytes, length);
		unpack_array(&row, &attrs);
		state->cells[i] = attrs.argc;
		for (j = 0; attrs.argc && j < 7; j++)
			state->attrs[i][j] = unpack_int(&attrs);
	}
}

static void
capture_web_snapshot(struct web_snapshot *state, u_int history, int fragmented)
{
	struct tmate_session session = { 0 }, *previous = tmate_session;
	struct tmate_decoder decoder;
	struct evbuffer *output;
	const unsigned char *bytes;
	char *buf;
	size_t length, offset = 0, capacity, count;

	tmate_session = &session;
	tmate_encoder_init(&session.websocket_encoder, NULL, NULL);
	tmate_send_websocket_snapshot(history);
	output = session.websocket_encoder.buffer;
	length = evbuffer_get_length(output);
	bytes = evbuffer_pullup(output, length);
	tmate_decoder_init(&decoder, read_web_snapshot, state);
	while (offset < length) {
		tmate_decoder_get_buffer(&decoder, &buf, &capacity);
		count = fragmented ? 1 : length - offset;
		if (count > capacity)
			count = capacity;
		memcpy(buf, bytes + offset, count);
		tmate_decoder_commit(&decoder, count);
		offset += count;
	}
	msgpack_unpacker_destroy(&decoder.unpacker);
	event_free(session.websocket_encoder.ev_buffer);
	evbuffer_free(output);
	tmate_session = previous;
}

static void
check_web_snapshot(int fragmented)
{
	struct web_snapshot state = { 0 };
	struct window_pane *wp = create_pane();
	struct session *s;
	struct winlink *wl;
	struct grid_cell gc = grid_default_cell;
	u_int id = wp->id, mode = wp->base.mode;
	int r = fragmented ? 0 : 1; /* bulk includes one history line */

	s = session_create(NULL, "web-snapshot", "/", environ_create(),
	    options_create(global_s_options), NULL);
	wl = winlink_add(&s->windows, 0);
	wl->session = s;
	winlink_set_window(wl, wp->window);
	s->curw = wl;
	window_set_active_pane(wp->window, wp, 0);
	utf8_set(&gc.data, 'A');
	grid_set_cell(wp->base.grid, 0, 0, &gc);
	grid_scroll_history(wp->base.grid, 8);
	utf8_set(&gc.data, 'B');
	grid_set_cell(wp->base.grid, 0, 1, &gc);
	grid_scroll_history(wp->base.grid, 8);
	gc = grid_default_cell;
	grid_set_tab(&gc, 1);
	gc.attr = GRID_ATTR_OVERLINE | GRID_ATTR_STRIKETHROUGH | GRID_ATTR_BRIGHT;
	gc.fg = COLOUR_FLAG_RGB | 0x123456;
	gc.bg = COLOUR_FLAG_RGB | 0x6543ab;
	grid_set_cell(wp->base.grid, 0, 2, &gc);
	gc = grid_default_cell;
	utf8_open(&gc.data, 0xe7);
	utf8_append(&gc.data, 0x95);
	utf8_append(&gc.data, 0x8c); /* 界 */
	gc.attr = GRID_ATTR_STRIKETHROUGH | GRID_ATTR_REVERSE;
	grid_set_cell(wp->base.grid, 1, 2, &gc);
	grid_set_padding(wp->base.grid, 2, 2, 8);
	gc = grid_default_cell;
	utf8_set(&gc.data, 0x1b);
	grid_set_cell(wp->base.grid, 3, 2, &gc);
	utf8_set(&gc.data, 0x7f);
	grid_set_cell(wp->base.grid, 4, 2, &gc);
	utf8_set(&gc.data, 0x00);
	grid_set_cell(wp->base.grid, 5, 2, &gc);
	utf8_set(&gc.data, 'X');
	gc.attr = GRID_ATTR_UNDERSCORE_5 | GRID_ATTR_ITALICS;
	gc.fg = COLOUR_FLAG_256 | 0xfe;
	gc.bg = COLOUR_FLAG_256 | 0xab;
	grid_set_cell(wp->base.grid, 6, 2, &gc);
	wp->base.cx = 7;
	wp->base.cy = 3;
	capture_web_snapshot(&state, r, fragmented);
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
	destroy_pane(wp);

	ASSERT_EQ(state.messages, 1);
	ASSERT(state.arity_ok);
	ASSERT_EQ(state.panes, 1);
	ASSERT_EQ(state.id, id);
	ASSERT_EQ(state.cx, 7);
	ASSERT_EQ(state.cy, 3);
	ASSERT_EQ(state.mode, mode);
	ASSERT_EQ(state.rows, 24 + r);
	if (r)
		ASSERT(state.text[0][0] == 'B');
	ASSERT(state.cells[r] >= 7);
	ASSERT_EQ(state.length[r], state.cells[r] + 2);
	/* UTF-8, padding sentinel and sanitized control bytes stay aligned. */
	ASSERT(memcmp(state.text[r], " 界!   X", 9) == 0);
	ASSERT_EQ(state.attrs[r][0], UINT32_C(0x8001ab56));
	ASSERT_EQ(state.attrs[r][1], UINT32_C(0x00100808));
	ASSERT_EQ(state.attrs[r][2], UINT32_C(0x04000808));
	ASSERT_EQ(state.attrs[r][3], UINT32_C(0x00000808));
	ASSERT_EQ(state.attrs[r][4], UINT32_C(0x00000808));
	ASSERT_EQ(state.attrs[r][5], UINT32_C(0x00000808));
	ASSERT_EQ(state.attrs[r][6], UINT32_C(0x0040abfe));
}

TEST(web_snapshot_fields_and_history)
{
	check_web_snapshot(0);
}

TEST(fragmented_web_snapshot_fields_without_history)
{
	check_web_snapshot(1);
}

TEST(empty_session_web_snapshot_is_complete)
{
	struct web_snapshot state = { 0 };
	capture_web_snapshot(&state, 0, 1);
	ASSERT_EQ(state.messages, 1);
	ASSERT(state.arity_ok);
	ASSERT_EQ(state.panes, 0);
}

static int snapshot_bulk;

TEST(host_snapshot_restores_grids_without_mutating_mode_backing)
{
	struct window_pane *first = create_pane(), *second = create_pane();
	struct window_mode_entry *copy;
	struct screen *backing;
	struct grid *old_backing;
	struct args *args = args_create();
	struct grid_cell gc = grid_default_cell;
	struct tmate_decoder decoder;
	char *buf;
	size_t capacity, count;
	int ch, before = dispatched;

	utf8_set(&gc.data, 'C');
	grid_set_cell(first->base.grid, 0, 0, &gc);
	window_pane_set_mode(first, first, &window_copy_mode, NULL, NULL, args);
	copy = TAILQ_FIRST(&first->modes);
	backing = copy->mode->get_screen(copy);
	old_backing = backing->grid;
	/* An existing alternate grid must be replaced/cleared without leaking. */
	first->base.saved_grid = grid_create(80, 24, 0);
	first->base.saved_flags = GRID_HISTORY;
	second->base.saved_grid = grid_create(80, 24, 0);
	second->base.saved_flags = GRID_HISTORY;
	tmate_decoder_init(&decoder, read_message, NULL);
	if (snapshot_bulk) {
		do {
			tmate_decoder_get_buffer(&decoder, &buf, &capacity);
			count = fread(buf, 1, capacity, stdin);
			tmate_decoder_commit(&decoder, count);
		} while (count != 0);
	} else {
		while ((ch = getchar()) != EOF) {
			tmate_decoder_get_buffer(&decoder, &buf, &capacity);
			ASSERT(capacity > 0);
			*buf = ch;
			tmate_decoder_commit(&decoder, 1);
		}
	}
	ASSERT(!ferror(stdin));
	ASSERT_EQ(dispatched, before + 1);
	ASSERT_EQ(first->base.cx, 80);
	ASSERT_EQ(first->base.cy, 3);
	ASSERT_EQ(first->base.grid->hsize, 1);
	ASSERT_EQ(first->base.saved_cx, 5);
	ASSERT_EQ(first->base.saved_cy, 2);
	ASSERT(first->base.saved_grid != NULL);
	ASSERT(!(first->base.grid->flags & GRID_HISTORY));
	ASSERT(first->base.saved_flags & GRID_HISTORY);
	grid_get_cell(first->base.grid, 0, 0, &gc);
	ASSERT_EQ(gc.data.data[0], 'H');
	grid_get_cell(first->base.grid, 0, 1, &gc);
	ASSERT(gc.flags & GRID_FLAG_TAB);
	ASSERT_EQ(gc.data.width, 1);
	ASSERT_EQ(gc.attr, GRID_ATTR_BRIGHT);
	grid_get_cell(first->base.grid, 1, 1, &gc);
	ASSERT_EQ(gc.data.width, 2);
	ASSERT_EQ(gc.attr, GRID_ATTR_REVERSE);
	ASSERT(memcmp(gc.data.data, "界", 3) == 0);
	grid_get_cell(first->base.grid, 2, 1, &gc);
	ASSERT(gc.flags & GRID_FLAG_PADDING);
	grid_get_cell(first->base.saved_grid, 0, 0, &gc);
	ASSERT_EQ(gc.data.data[0], 'S');
	ASSERT(second->base.saved_grid == NULL);
	ASSERT(second->base.grid->flags & GRID_HISTORY);
	grid_get_cell(second->base.grid, 0, 0, &gc);
	ASSERT_EQ(gc.data.data[0], 'B');
	ASSERT(TAILQ_FIRST(&first->modes) == copy);
	ASSERT(backing->grid == old_backing);
	grid_get_cell(backing->grid, 0, 0, &gc);
	ASSERT_EQ(gc.data.data[0], 'C');
	ASSERT(first->screen == copy->screen);
	ASSERT(first->flags & PANE_REDRAW);
	window_pane_reset_mode(first);
	args_free(args);
	ASSERT(first->screen == &first->base);
	/* Restored alternate state must work through production screen lifecycle. */
	screen_alternate_off(&first->base, NULL, 1);
	ASSERT(first->base.saved_grid == NULL);
	ASSERT_EQ(first->base.cx, 5);
	ASSERT_EQ(first->base.cy, 2);
	ASSERT(first->base.grid->flags & GRID_HISTORY);
	grid_get_cell(first->base.grid, 0, first->base.grid->hsize, &gc);
	ASSERT_EQ(gc.data.data[0], 'S');
	msgpack_unpacker_destroy(&decoder.unpacker);
	destroy_pane(second);
	destroy_pane(first);
}

TEST(host_layout_snapshot_and_following_pty_restore_real_session)
{
	struct tmate_decoder decoder;
	struct session *s;
	struct window_pane *first, *second;
	struct grid_cell gc;
	char *buf;
	size_t capacity, count;
	int ch, before = dispatched;

	ASSERT(RB_EMPTY(&sessions));
	tmate_decoder_init(&decoder, read_message, tmate_session);
	if (snapshot_bulk) {
		do {
			tmate_decoder_get_buffer(&decoder, &buf, &capacity);
			count = fread(buf, 1, capacity, stdin);
			tmate_decoder_commit(&decoder, count);
		} while (count != 0);
	} else {
		while ((ch = getchar()) != EOF) {
			tmate_decoder_get_buffer(&decoder, &buf, &capacity);
			ASSERT(capacity > 0);
			*buf = ch;
			tmate_decoder_commit(&decoder, 1);
		}
	}
	ASSERT(!ferror(stdin));
	ASSERT_EQ(dispatched, before + 4);
	s = RB_MIN(sessions, &sessions);
	ASSERT(s != NULL);
	ASSERT_EQ(winlink_count(&s->windows), 2);
	first = window_pane_find_by_id(0);
	second = window_pane_find_by_id(1);
	ASSERT(first != NULL && second != NULL);
	ASSERT(first->window == winlink_find_by_index(&s->windows, 0)->window);
	ASSERT(second->window == winlink_find_by_index(&s->windows, 1)->window);
	ASSERT(s->curw->window == first->window);
	ASSERT_EQ(first->sx, 67);
	ASSERT_EQ(first->sy, 19);
	ASSERT_EQ(second->sx, 43);
	ASSERT_EQ(second->sy, 12);
	ASSERT_EQ(second->xoff, 7);
	ASSERT_EQ(second->yoff, 4);
	ASSERT(first->layout_cell != NULL && second->layout_cell != NULL);
	ASSERT(first->ictx != NULL && second->ictx != NULL);
	ASSERT_EQ(first->base.grid->hsize, 1);
	grid_get_cell(first->base.grid, 0, 0, &gc);
	ASSERT_EQ(gc.data.data[0], 'H');
	grid_get_cell(first->base.grid, 1, 1, &gc);
	ASSERT(memcmp(gc.data.data, "界", 3) == 0);
	ASSERT(first->base.saved_grid != NULL);
	ASSERT_EQ(first->base.saved_grid->sx, 67);
	ASSERT_EQ(first->base.saved_grid->sy, 19);
	ASSERT_EQ(first->base.saved_cx, 5);
	ASSERT_EQ(first->base.saved_cy, 2);
	grid_get_cell(first->base.saved_grid, 0, 0, &gc);
	ASSERT_EQ(gc.data.data[0], 'S');
	ASSERT(second->base.saved_grid == NULL);
	grid_get_cell(second->base.grid, 0, 0, &gc);
	ASSERT_EQ(gc.data.data[0], 'B');
	/* Subsequent bytes go through production input parsing on restored grids. */
	grid_get_cell(first->base.grid, 3, first->base.grid->hsize + 9, &gc);
	ASSERT_EQ(gc.data.data[0], 'Z');
	grid_get_cell(second->base.grid, 2, 1, &gc);
	ASSERT_EQ(gc.data.data[0], 'Y');
	ASSERT_EQ(first->base.cx, 4);
	ASSERT_EQ(first->base.cy, 9);
	ASSERT_EQ(second->base.cx, 3);
	ASSERT_EQ(second->base.cy, 1);
	msgpack_unpacker_destroy(&decoder.unpacker);
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
	ASSERT(RB_EMPTY(&sessions));
}

static int split_mixed, split_vertical, split_layouts, split_snapshots, split_output;
static struct window_pane *split_first, *split_second, *split_third;

static void
read_split_message(void *session, struct tmate_unpacker *uk)
{
	struct tmate_unpacker original = *uk;
	struct session *s;
	struct window *w;
	struct window_pane *wp, *ordered;
	struct layout_cell *leaf, *root;
	struct grid_cell gc;
	msgpack_object *panes, *pane;
	u_int i, n, opcode = uk->argv[0].via.u64;

	tmate_dispatch_daemon_message(session, uk);
	dispatched++;
	s = RB_MIN(sessions, &sessions);
	ASSERT(s != NULL);
	w = s->curw->window;
	root = w->layout_root;
	ASSERT(root != NULL);
	if (opcode == TMATE_OUT_SYNC_LAYOUT) {
		split_layouts++;
		ASSERT_EQ(winlink_count(&s->windows), 1);
		ASSERT_EQ(w->sx, split_layouts == 1 ? 80 : 91);
		ASSERT_EQ(w->sy, split_layouts == 1 ? 24 : 31);
		/* Compare each production leaf against actual encoded rectangles. */
		panes = original.argv[3].via.array.ptr[0].via.array.ptr[2].via.array.ptr;
		n = original.argv[3].via.array.ptr[0].via.array.ptr[2].via.array.size;
		ASSERT_EQ(n, (split_layouts < 3 ? 2 : 1) + split_mixed);
		ordered = TAILQ_FIRST(&w->panes);
		for (i = 0; i < n; i++) {
			pane = panes[i].via.array.ptr;
			wp = window_pane_find_by_id(pane[0].via.u64);
			ASSERT(wp != NULL && wp->window == w && wp->ictx != NULL);
			ASSERT(wp == ordered);
			ordered = TAILQ_NEXT(ordered, entry);
			leaf = wp->layout_cell;
			ASSERT(leaf != NULL && leaf->wp == wp);
			ASSERT_EQ(wp->sx, pane[1].via.u64);
			ASSERT_EQ(wp->sy, pane[2].via.u64);
			ASSERT_EQ(wp->xoff, pane[3].via.u64);
			ASSERT_EQ(wp->yoff, pane[4].via.u64);
			ASSERT_EQ(leaf->g.sx, wp->sx);
			ASSERT_EQ(leaf->g.sy, wp->sy);
			ASSERT_EQ(leaf->g.xoff, wp->xoff);
			ASSERT_EQ(leaf->g.yoff, wp->yoff);
			ASSERT(wp->screen == &wp->base);
		}
		if (split_layouts == 1) {
			split_first = window_pane_find_by_id(0);
			split_second = window_pane_find_by_id(1);
			if (split_mixed)
				split_third = window_pane_find_by_id(2);
		}
		ASSERT(window_pane_find_by_id(1) == split_second);
		ASSERT(w->active == split_second);
		ASSERT_EQ(root->g.sx, w->sx);
		ASSERT_EQ(root->g.sy, w->sy);
		ASSERT_EQ(root->g.xoff, 0);
		ASSERT_EQ(root->g.yoff, 0);
		if (split_layouts < 3 || split_mixed) {
			ASSERT_EQ(root->type, split_vertical ? LAYOUT_TOPBOTTOM : LAYOUT_LEFTRIGHT);
			leaf = TAILQ_FIRST(&root->cells);
			ASSERT(leaf == split_second->layout_cell && leaf->parent == root);
			ASSERT(layout_search_by_border(root,
			    split_vertical ? 0 : leaf->g.sx,
			    split_vertical ? leaf->g.sy : 0) == leaf);
			leaf = TAILQ_NEXT(leaf, entry);
			ASSERT(leaf != NULL && leaf->parent == root);
			ASSERT(TAILQ_NEXT(leaf, entry) == NULL);
			if (split_layouts < 3) {
				ASSERT(window_pane_find_by_id(0) == split_first);
				if (split_mixed) {
					ASSERT(window_pane_find_by_id(2) == split_third);
					ASSERT_EQ(leaf->type, split_vertical ? LAYOUT_LEFTRIGHT : LAYOUT_TOPBOTTOM);
					ASSERT(TAILQ_FIRST(&leaf->cells) == split_third->layout_cell);
					ASSERT(TAILQ_NEXT(TAILQ_FIRST(&leaf->cells), entry) == split_first->layout_cell);
					ASSERT(split_first->layout_cell->parent == leaf);
					ASSERT(split_third->layout_cell->parent == leaf);
					ASSERT_EQ(leaf->g.sx, split_vertical ? w->sx : split_first->sx);
					ASSERT_EQ(leaf->g.sy, split_vertical ? split_first->sy : w->sy);
					ASSERT(layout_search_by_border(root,
					    split_vertical ? split_third->sx : leaf->g.xoff,
					    split_vertical ? leaf->g.yoff : split_third->sy) == split_third->layout_cell);
				} else
					ASSERT(leaf == split_first->layout_cell);
			} else {
				ASSERT(window_pane_find_by_id(0) == NULL);
				ASSERT(window_pane_find_by_id(2) == split_third);
				ASSERT(leaf == split_third->layout_cell);
			}
		} else {
			ASSERT(window_pane_find_by_id(0) == NULL);
			ASSERT(root == split_second->layout_cell && root->parent == NULL);
			ASSERT_EQ(root->type, LAYOUT_WINDOWPANE);
			ASSERT(TAILQ_NEXT(TAILQ_FIRST(&w->panes), entry) == NULL);
		}
	} else if (opcode == TMATE_OUT_SNAPSHOT) {
		split_snapshots++;
		TAILQ_FOREACH(wp, &w->panes, entry) {
			ASSERT_EQ(wp->base.grid->sx, wp->sx);
			ASSERT_EQ(wp->base.grid->sy, wp->sy);
			grid_get_cell(wp->base.grid, 0, wp->base.grid->hsize, &gc);
			ASSERT_EQ(gc.data.data[0], wp->id == 0 ? 'A' : wp->id == 1 ? 'B' : 'C');
		}
	} else if (opcode == TMATE_OUT_PTY_DATA) {
		split_output++;
		wp = window_pane_find_by_id(original.argv[1].via.u64);
		ASSERT(wp != NULL);
		grid_get_cell(wp->base.grid, 0, wp->base.grid->hsize, &gc);
		ASSERT_EQ(gc.data.data[0], wp->id == 0 ? 'Z' : wp->id == 1 ? 'Y' : 'W');
		ASSERT_EQ(wp->base.cx, 1);
		ASSERT_EQ(wp->base.cy, 0);
	}
}

TEST(host_split_reconnect_resize_and_removal)
{
	struct tmate_decoder decoder;
	struct session *s;
	char *buf;
	size_t capacity, count;
	int ch, before = dispatched, failures = _tests_failed;

	ASSERT(RB_EMPTY(&sessions));
	tmate_decoder_init(&decoder, read_split_message, tmate_session);
	if (snapshot_bulk) {
		do {
			tmate_decoder_get_buffer(&decoder, &buf, &capacity);
			count = fread(buf, 1, capacity, stdin);
			tmate_decoder_commit(&decoder, count);
		} while (count != 0);
	} else {
		while ((ch = getchar()) != EOF) {
			tmate_decoder_get_buffer(&decoder, &buf, &capacity);
			*buf = ch;
			tmate_decoder_commit(&decoder, 1);
		}
	}
	ASSERT(!ferror(stdin));
	ASSERT_EQ(_tests_failed, failures);
	ASSERT_EQ(dispatched, before + 11 + 3 * split_mixed);
	ASSERT_EQ(split_layouts, 3);
	ASSERT_EQ(split_snapshots, 3);
	ASSERT_EQ(split_output, 5 + 3 * split_mixed);
	msgpack_unpacker_destroy(&decoder.unpacker);
	s = RB_MIN(sessions, &sessions);
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
	ASSERT(RB_EMPTY(&sessions));
}

/* Exercise the protocol pane limit and unsuccessful partial tree cleanup. */
static void
send_rectangle_layout(u_int count, int variant)
{
	struct tmate_decoder decoder;
	msgpack_sbuffer sb;
	msgpack_packer pk;
	char *buf;
	size_t capacity, offset = 0, size;
	u_int i, id, x, y, sx, sy;

	msgpack_sbuffer_init(&sb);
	msgpack_packer_init(&pk, &sb, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, 5);
	msgpack_pack_int(&pk, TMATE_OUT_SYNC_LAYOUT);
	msgpack_pack_int(&pk, variant < 0 ? 2 * count - 1 : 80);
	msgpack_pack_int(&pk, variant < 0 ? 1 : 24);
	msgpack_pack_array(&pk, 1);
	msgpack_pack_array(&pk, 4);
	msgpack_pack_int(&pk, 0);
	msgpack_pack_str(&pk, 1);
	msgpack_pack_str_body(&pk, "w", 1);
	msgpack_pack_array(&pk, count);
	for (i = 0; i < count; i++) {
		id = count - 1 - i;
		x = variant < 0 ? 2 * id : variant == 0 ? 0 :
		    variant == 1 ? (id == 0 ? 0 : 11) : 12 * id;
		y = variant == 1 && id == 2 ? 8 : 0;
		sx = variant < 0 ? 1 : 10;
		sy = variant < 0 ? 1 : variant == 1 && id == 1 ? 3 : 12;
		msgpack_pack_array(&pk, 5);
		msgpack_pack_uint32(&pk, id);
		msgpack_pack_uint32(&pk, sx);
		msgpack_pack_uint32(&pk, sy);
		msgpack_pack_uint32(&pk, x);
		msgpack_pack_uint32(&pk, y);
	}
	msgpack_pack_int(&pk, 0);
	msgpack_pack_int(&pk, 0);
	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, TMATE_OUT_PTY_DATA);
	msgpack_pack_int(&pk, 0);
	msgpack_pack_str(&pk, 1);
	msgpack_pack_str_body(&pk, "X", 1);
	tmate_decoder_init(&decoder, read_message, tmate_session);
	while (offset < sb.size) {
		tmate_decoder_get_buffer(&decoder, &buf, &capacity);
		size = sb.size - offset;
		if (size > capacity)
			size = capacity;
		memcpy(buf, sb.data + offset, size);
		tmate_decoder_commit(&decoder, size);
		offset += size;
	}
	msgpack_unpacker_destroy(&decoder.unpacker);
	msgpack_sbuffer_destroy(&sb);
}

static void
remove_rectangle_session(void)
{
	struct session *s = RB_MIN(sessions, &sessions);

	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
}

TEST(maximum_panes_recover_spatial_tree)
{
	struct session *s;
	struct window *w;
	struct window_pane *wp;
	struct layout_cell *lc;
	struct grid_cell gc;
	u_int i;

	ASSERT(RB_EMPTY(&sessions));
	send_rectangle_layout(1000, -1);
	s = RB_MIN(sessions, &sessions);
	ASSERT(s != NULL);
	w = s->curw->window;
	ASSERT_EQ(TAILQ_FIRST(&w->panes)->id, 999);
	ASSERT_EQ(w->layout_root->type, LAYOUT_LEFTRIGHT);
	ASSERT_EQ(w->layout_root->g.sx, 1999);
	lc = TAILQ_FIRST(&w->layout_root->cells);
	for (i = 0; i < 1000; i++) {
		wp = window_pane_find_by_id(i);
		ASSERT(wp != NULL && wp->ictx != NULL);
		ASSERT(lc == wp->layout_cell);
		ASSERT(lc->parent == w->layout_root);
		ASSERT_EQ(lc->type, LAYOUT_WINDOWPANE);
		ASSERT_EQ(lc->g.sx, 1);
		ASSERT_EQ(lc->g.xoff, 2 * i);
		lc = TAILQ_NEXT(lc, entry);
	}
	ASSERT(lc == NULL);
	wp = window_pane_find_by_id(0);
	grid_get_cell(wp->base.grid, 0, wp->base.grid->hsize, &gc);
	ASSERT_EQ(gc.data.data[0], 'X');
	remove_rectangle_session();
	ASSERT(RB_EMPTY(&sessions));
}

TEST(non_tiled_rectangles_preserve_legacy_fallback)
{
	struct session *s;
	struct window *w;
	struct window_pane *wp;
	struct layout_cell *lc;
	int variant;

	ASSERT(RB_EMPTY(&sessions));
	for (variant = 0; variant < 3; variant++) {
		send_rectangle_layout(3, variant);
		s = RB_MIN(sessions, &sessions);
		ASSERT(s != NULL);
		w = s->curw->window;
		ASSERT_EQ(w->layout_root->type, LAYOUT_LEFTRIGHT);
		ASSERT_EQ(w->layout_root->g.sx, 80);
		ASSERT_EQ(w->layout_root->g.sy, 24);
		lc = TAILQ_FIRST(&w->layout_root->cells);
		TAILQ_FOREACH(wp, &w->panes, entry) {
			ASSERT(lc == wp->layout_cell && lc->wp == wp);
			ASSERT(lc->parent == w->layout_root);
			ASSERT_EQ(lc->type, LAYOUT_WINDOWPANE);
			ASSERT_EQ(lc->g.sx, wp->sx);
			ASSERT_EQ(lc->g.sy, wp->sy);
			ASSERT_EQ(lc->g.xoff, wp->xoff);
			ASSERT_EQ(lc->g.yoff, wp->yoff);
			lc = TAILQ_NEXT(lc, entry);
		}
		ASSERT(lc == NULL);
		remove_rectangle_session();
		ASSERT(RB_EMPTY(&sessions));
	}
}

/* A later invalid pane must not resize the earlier valid pane. */
static void
send_invalid_layout(struct window_pane *wp, int variant)
{
	msgpack_sbuffer sb;
	msgpack_packer pk;
	struct tmate_decoder decoder;
	char *buf;
	size_t capacity, offset;
	int i, j;

	msgpack_sbuffer_init(&sb);
	msgpack_packer_init(&pk, &sb, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, variant == 0 ? 4 : 5);
	msgpack_pack_int(&pk, TMATE_OUT_SYNC_LAYOUT);
	msgpack_pack_int64(&pk, variant == 1 ? -1 : variant == 2 ? 0 :
	    variant == 3 ? WINDOW_MAXIMUM + 1 : 80);
	msgpack_pack_int(&pk, 24);
	msgpack_pack_array(&pk, variant == 4 ? 513 : 1);
	for (i = 0; i < (variant == 4 ? 513 : 1); i++) {
		msgpack_pack_array(&pk, 4);
		msgpack_pack_int64(&pk, variant == 5 ? (int64_t)UINT_MAX + 1 : i);
		if (variant == 6)
			msgpack_pack_nil(&pk);
		else {
			msgpack_pack_str(&pk, 1);
			msgpack_pack_str_body(&pk, "w", 1);
		}
		msgpack_pack_array(&pk, variant == 7 ? 0 : 2);
		for (j = 0; j < (variant == 7 ? 0 : 2); j++) {
			msgpack_pack_array(&pk, variant == 8 && j == 1 ? 4 : 5);
			msgpack_pack_int64(&pk, j == 0 || variant == 9 ? wp->id :
			    variant == 10 ? (int64_t)UINT_MAX + 1 + wp->id :
			    variant == 11 ? -1 : wp->id + 1);
			msgpack_pack_int64(&pk, j == 1 && variant == 12 ? -1 :
			    j == 1 && variant == 13 ? 0 : 40);
			msgpack_pack_int64(&pk, j == 1 && variant == 14 ? WINDOW_MAXIMUM + 1 : 12);
			msgpack_pack_uint64(&pk, j == 1 && variant == 15 ? UINT64_MAX : 0);
			if (!(variant == 8 && j == 1))
				msgpack_pack_int(&pk, j == 1 && variant == 16 ? WINDOW_MAXIMUM : 0);
		}
		msgpack_pack_int64(&pk, variant == 17 ? (int64_t)UINT_MAX + 1 : wp->id);
	}
	if (variant != 0)
		msgpack_pack_int64(&pk, variant == 18 ? -2 : 0);
	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, TMATE_OUT_WRITE_COPY_MODE);
	msgpack_pack_uint32(&pk, wp->id);
	msgpack_pack_str(&pk, 5);
	msgpack_pack_str_body(&pk, "after", 5);
	tmate_decoder_init(&decoder, read_message, tmate_session);
	for (offset = 0; offset < sb.size; offset++) {
		tmate_decoder_get_buffer(&decoder, &buf, &capacity);
		*buf = sb.data[offset];
		tmate_decoder_commit(&decoder, 1);
	}
	msgpack_unpacker_destroy(&decoder.unpacker);
	msgpack_sbuffer_destroy(&sb);
}

TEST(malformed_layout_preserves_session_geometry_and_following_message)
{
	struct window_pane *wp = create_pane();
	struct session *s;
	struct winlink *wl;
	struct grid *grid = wp->base.grid;
	struct layout_cell *root = wp->window->layout_root;
	int variant, before;

	s = session_create(NULL, "invalid-layout", "/", environ_create(),
	    options_create(global_s_options), NULL);
	wl = winlink_add(&s->windows, 0);
	wl->session = s;
	winlink_set_window(wl, wp->window);
	s->curw = wl;
	for (variant = 0; variant < 19; variant++) {
		before = dispatched;
		send_invalid_layout(wp, variant);
		ASSERT_EQ(dispatched, before + 2);
		ASSERT(RB_MIN(sessions, &sessions) == s);
		ASSERT(s->curw == wl);
		ASSERT_EQ(winlink_count(&s->windows), 1);
		ASSERT(wp->base.grid == grid);
		ASSERT(wp->window->layout_root == root);
		ASSERT_EQ(wp->sx, 80);
		ASSERT_EQ(wp->sy, 24);
		ASSERT_EQ(wp->xoff, 0);
		ASSERT_EQ(wp->yoff, 0);
		ASSERT(window_pane_find_by_id(wp->id + 1) == NULL);
		ASSERT(line_equals(wp, variant, "after"));
	}
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
	destroy_pane(wp);
}

/* Each malformed candidate is followed by valid output on the same stream. */
static void
invalid_snapshot(struct window_pane *wp, int variant)
{
	msgpack_sbuffer sb;
	msgpack_packer pk;
	struct tmate_decoder decoder;
	char *buf;
	size_t capacity, offset;
	int y, j, cells;
	int lines = variant == 14 ? 2025 : variant == 4 ? 23 : 24;
	char oversize[82];
	const char *text = variant == 6 ? "\xe7\x95" :
	    variant == 7 ? "界!" : variant == 8 ? "\033" :
	    variant == 9 ? "e\xcc\x81" : variant == 16 ? "!" :
	    variant == 17 ? "  !" : variant == 18 ? "\xc0\xaf" :
	    variant == 19 ? "界" : variant == 23 ? "AB" : "A";

	memset(oversize, 'A', 81);
	oversize[81] = '\0';
	if (variant == 15)
		text = oversize;
	msgpack_sbuffer_init(&sb);
	msgpack_packer_init(&pk, &sb, msgpack_sbuffer_write);
	msgpack_pack_array(&pk, 2);
	msgpack_pack_int(&pk, TMATE_OUT_SNAPSHOT);
	msgpack_pack_array(&pk, 1);
	msgpack_pack_array(&pk, variant == 12 ? 3 : 4);
	msgpack_pack_int64(&pk, variant == 0 ? (int64_t)UINT_MAX + 1 + wp->id :
	    variant == 1 ? (int64_t)wp->id - ((int64_t)UINT_MAX + 1) : wp->id);
	msgpack_pack_uint64(&pk, variant == 13 ? UINT64_MAX : MODE_CURSOR);
	if (variant == 20)
		msgpack_pack_nil(&pk); /* invalid base screen type */
	else {
		msgpack_pack_array(&pk, 3);
		msgpack_pack_int64(&pk, variant == 2 ? 81 : variant == 22 ? -1 : 0);
		msgpack_pack_int(&pk, variant == 3 ? 24 : 0);
		msgpack_pack_array(&pk, lines);
		for (y = 0; y < lines; y++) {
			msgpack_pack_array(&pk, 2);
			msgpack_pack_str(&pk, y == 0 ? strlen(text) : 0);
			if (y == 0)
				msgpack_pack_str_body(&pk, text, strlen(text));
			cells = y != 0 ? 0 : variant == 15 ? 81 :
			    variant == 5 || variant == 7 || variant == 17 || variant == 19 ? 2 : 1;
			msgpack_pack_array(&pk, cells);
			if (y == 0) {
				if (variant == 21)
					msgpack_pack_int(&pk, -1);
				else
					msgpack_pack_uint64(&pk, variant == 10 ? UINT64_MAX :
					    variant == 16 ? 0x04000808 : variant == 17 ? 0x80000808 : 0x00000808);
				if (variant == 15) {
					for (j = 1; j < cells; j++)
						msgpack_pack_uint32(&pk, 0x00000808);
				}
				if (variant == 5 || variant == 7 || variant == 17 || variant == 19)
					msgpack_pack_uint32(&pk, variant == 17 || variant == 19 ? 0x04000808 : 0x00000808);
			}
		}
	}
	if (variant != 12) {
		if (variant == 11)
			msgpack_pack_int(&pk, 0); /* valid base, invalid saved grid */
		else
			msgpack_pack_nil(&pk);
	}
	msgpack_pack_array(&pk, 3);
	msgpack_pack_int(&pk, TMATE_OUT_WRITE_COPY_MODE);
	msgpack_pack_uint32(&pk, wp->id);
	msgpack_pack_str(&pk, 5);
	msgpack_pack_str_body(&pk, "after", 5);
	tmate_decoder_init(&decoder, read_message, NULL);
	for (offset = 0; offset < sb.size; offset++) {
		tmate_decoder_get_buffer(&decoder, &buf, &capacity);
		*buf = sb.data[offset];
		tmate_decoder_commit(&decoder, 1);
	}
	msgpack_unpacker_destroy(&decoder.unpacker);
	msgpack_sbuffer_destroy(&sb);
}

TEST(malformed_snapshot_preserves_both_grids_and_message_boundaries)
{
	int variant, before;
	struct window_pane *wp;
	struct grid *base, *saved;
	struct grid_cell gc = grid_default_cell;

	for (variant = 0; variant < 24; variant++) {
		wp = create_pane();
		utf8_set(&gc.data, 'P');
		grid_set_cell(wp->base.grid, 0, 0, &gc);
		wp->base.cx = 4;
		wp->base.cy = 3;
		wp->base.saved_grid = grid_create(80, 24, 0);
		base = wp->base.grid;
		saved = wp->base.saved_grid;
		before = dispatched;
		invalid_snapshot(wp, variant);
		ASSERT_EQ(dispatched, before + 2);
		ASSERT(wp->base.grid == base);
		ASSERT(wp->base.saved_grid == saved);
		ASSERT_EQ(wp->base.cx, 4);
		ASSERT_EQ(wp->base.cy, 3);
		grid_get_cell(base, 0, 0, &gc);
		ASSERT_EQ(gc.data.data[0], 'P');
		ASSERT(line_equals(wp, 0, "after"));
		destroy_pane(wp);
	}
}

static int host_exit;
static int host_bulk;

TEST(host_frozen_copy_and_live_base_restore_independently)
{
	struct tmate_decoder decoder;
	struct session *s;
	struct window_pane *wp;
	struct window_mode_entry *wme;
	struct grid_cell gc;
	char *buf;
	size_t capacity, count;
	int ch, before = dispatched;

	ASSERT(RB_EMPTY(&sessions));
	tmate_decoder_init(&decoder, read_message, tmate_session);
	while (1) {
		tmate_decoder_get_buffer(&decoder, &buf, &capacity);
		if (host_bulk) {
			count = fread(buf, 1, capacity, stdin);
			if (count == 0)
				break;
		} else {
			ch = getchar();
			if (ch == EOF)
				break;
			*buf = ch;
			count = 1;
		}
		tmate_decoder_commit(&decoder, count);
	}
	ASSERT(!ferror(stdin));
	ASSERT_EQ(dispatched, before + 5);
	wp = window_pane_find_by_id(0);
	ASSERT(wp != NULL);
	wme = TAILQ_FIRST(&wp->modes);
	ASSERT(wme != NULL && wme->mode == &window_copy_mode);
	grid_get_cell(wme->mode->get_screen(wme)->grid, 0, 1, &gc);
	ASSERT_EQ(gc.data.data[0], 'F');
	grid_get_cell(wp->base.grid, 0, 1, &gc);
	ASSERT_EQ(gc.data.data[0], 'L');
	ASSERT(wp->base.saved_grid != NULL);
	grid_get_cell(wp->base.saved_grid, 0, 0, &gc);
	ASSERT_EQ(gc.data.data[0], 'S');
	ASSERT_EQ(options_get_number(wp->window->options, "window-size"), WINDOW_SIZE_MANUAL);
	ASSERT_EQ(wp->window->manual_sx, 80);
	ASSERT_EQ(wp->window->manual_sy, 24);
	window_pane_reset_mode(wp);
	ASSERT(wp->screen == &wp->base);
	grid_get_cell(wp->screen->grid, 0, 1, &gc);
	ASSERT_EQ(gc.data.data[0], 'L');
	msgpack_unpacker_destroy(&decoder.unpacker);
	s = RB_MIN(sessions, &sessions);
	session_destroy(s, 0, __func__);
	while (cmdq_next(NULL) != 0)
		;
}

TEST(host_copy_state_routes_and_preserves_clock)
{
	struct window_pane *wp = create_pane();
	struct grid_cell gc = grid_default_cell;
	struct window_copy_state state = { 0 };
	struct tmate_decoder decoder;
	char *buf;
	size_t capacity, count;
	int ch, before = dispatched;

	utf8_set(&gc.data, 'H');
	grid_set_cell(wp->base.grid, 0, wp->base.grid->sy - 1, &gc);
	grid_scroll_history(wp->base.grid, 1);
	window_pane_set_mode(wp, NULL, &window_clock_mode, NULL, NULL, NULL);
	tmate_decoder_init(&decoder, read_message, NULL);
	while (1) {
		tmate_decoder_get_buffer(&decoder, &buf, &capacity);
		ASSERT(capacity > 0);
		if (host_bulk) {
			count = fread(buf, 1, capacity, stdin);
			if (count == 0)
				break;
			tmate_decoder_commit(&decoder, count);
		} else {
			while ((ch = getchar()) != EOF) {
				*buf = ch;
				tmate_decoder_commit(&decoder, 1);
				break;
			}
			if (ch == EOF)
				break;
		}
	}
	ASSERT(feof(stdin));
	ASSERT(!ferror(stdin));
	ASSERT_EQ(dispatched, before + 1);
	ASSERT(TAILQ_FIRST(&wp->modes)->mode == &window_copy_mode);
	ASSERT(TAILQ_NEXT(TAILQ_FIRST(&wp->modes), entry) != NULL);
	ASSERT(TAILQ_NEXT(TAILQ_FIRST(&wp->modes), entry)->mode ==
	    &window_clock_mode);
	ASSERT(window_copy_get_state(wp, &state));
	ASSERT_EQ(state.view_mode, 0);
	ASSERT_EQ(state.cx, 5);
	ASSERT_EQ(state.cy, 6);
	ASSERT_EQ(state.selection, 1);
	ASSERT_EQ(state.selx, 3);
	ASSERT_EQ(state.transformed_sely, 0);
	ASSERT_EQ(state.rectangle, 1);
	msgpack_unpacker_destroy(&decoder.unpacker);
	destroy_pane(wp);
}

TEST(host_encoder_output_routes_without_feedback)
{
	struct window_pane *first = create_pane(), *second = create_pane();
	struct tmate_decoder decoder;
	char *buf;
	size_t capacity;
	int ch, before = dispatched;

	window_pane_set_mode(first, NULL, &window_clock_mode, NULL, NULL, NULL);
	tmate_decoder_init(&decoder, read_message, NULL);
	/* Exercise every streaming boundary using the actual host encoder bytes. */
	while ((ch = getchar()) != EOF) {
		tmate_decoder_get_buffer(&decoder, &buf, &capacity);
		ASSERT(capacity > 0);
		*buf = ch;
		tmate_decoder_commit(&decoder, 1);
	}
	ASSERT(!ferror(stdin));
	ASSERT_EQ(dispatched, before + (host_exit ? 5 : 4));
	ASSERT(line_equals(second, 0, "other pane"));
	if (!host_exit) {
		ASSERT(TAILQ_FIRST(&first->modes)->mode == &window_view_mode);
		ASSERT(line_equals(first, 0, "literal %s 38"));
		ASSERT(line_equals(first, 1, "second ⚡"));
		window_pane_reset_mode(first);
	}
	ASSERT(TAILQ_FIRST(&first->modes)->mode == &window_clock_mode);
	ASSERT(first->screen == TAILQ_FIRST(&first->modes)->screen);
	/* Relay lifecycle runs without a host encoder; its hook is excluded. */
	msgpack_unpacker_destroy(&decoder.unpacker);
	destroy_pane(second);
	destroy_pane(first);
}

#include "test-layout.h"
#include "test-snapshot.h"
#include "test-snapshot-pane.h"
#include "test-snapshot-media.h"
#include "test-snapshot-wire.h"

int
main(int argc, char **argv)
{
	const struct options_table_entry *oe;
	if (setlocale(LC_CTYPE, "C.UTF-8") == NULL)
		setlocale(LC_CTYPE, "en_US.UTF-8");
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
	if (argc == 2 && (strcmp(argv[1], "--host-pane-codec") == 0 ||
	    strcmp(argv[1], "--host-pane-codec-bulk") == 0)) {
		snapshot_bulk = strstr(argv[1], "-bulk") != NULL;
		RUN_TEST(host_pane_codec_stream_roundtrip);
	} else if (argc == 2 && (strcmp(argv[1], "--host-grid-codec") == 0 ||
	    strcmp(argv[1], "--host-grid-codec-bulk") == 0)) {
		snapshot_bulk = strstr(argv[1], "-bulk") != NULL;
		RUN_TEST(host_grid_codec_stream_roundtrip);
	} else if (argc == 2 && (strcmp(argv[1], "--host-frozen-copy") == 0 ||
	    strcmp(argv[1], "--host-frozen-copy-bulk") == 0)) {
		host_bulk = strcmp(argv[1], "--host-frozen-copy-bulk") == 0;
		RUN_TEST(host_frozen_copy_and_live_base_restore_independently);
	} else if (argc == 2 && (strstr(argv[1], "--host-vertical-reconnect") == argv[1] ||
	    strstr(argv[1], "--host-horizontal-reconnect") == argv[1] ||
	    strstr(argv[1], "--host-mixed-vertical-reconnect") == argv[1] ||
	    strstr(argv[1], "--host-mixed-horizontal-reconnect") == argv[1])) {
		snapshot_bulk = strstr(argv[1], "-bulk") != NULL;
		split_vertical = strstr(argv[1], "-vertical-") != NULL;
		split_mixed = strstr(argv[1], "-mixed-") != NULL;
		RUN_TEST(host_split_reconnect_resize_and_removal);
	} else if (argc == 2 && (strcmp(argv[1], "--host-layout-snapshot") == 0 ||
	    strcmp(argv[1], "--host-layout-snapshot-bulk") == 0)) {
		snapshot_bulk = strcmp(argv[1], "--host-layout-snapshot-bulk") == 0;
		RUN_TEST(host_layout_snapshot_and_following_pty_restore_real_session);
	} else if (argc == 2 && (strcmp(argv[1], "--host-snapshot") == 0 ||
	    strcmp(argv[1], "--host-snapshot-bulk") == 0)) {
		snapshot_bulk = strcmp(argv[1], "--host-snapshot-bulk") == 0;
		RUN_TEST(host_snapshot_restores_grids_without_mutating_mode_backing);
	} else if (argc == 2 && (strcmp(argv[1], "--host-output") == 0 ||
	    strcmp(argv[1], "--host-exit") == 0 ||
	    strcmp(argv[1], "--host-copy-state") == 0 ||
	    strcmp(argv[1], "--host-copy-state-bulk") == 0)) {
		host_exit = strcmp(argv[1], "--host-exit") == 0;
		host_bulk = strcmp(argv[1], "--host-copy-state-bulk") == 0;
		if (host_exit || strcmp(argv[1], "--host-output") == 0)
			RUN_TEST(host_encoder_output_routes_without_feedback);
		else
			RUN_TEST(host_copy_state_routes_and_preserves_clock);
	} else {
		RUN_TEST(pane_snapshot_rejects_unsafe_metadata_before_decode);
		RUN_TEST(pane_snapshot_preserves_safe_metadata_across_repeated_roundtrips);
		RUN_TEST(pane_snapshot_metadata_limits_and_embedded_nul_are_checked);
#ifdef ENABLE_SIXEL
		RUN_TEST(pane_snapshot_clears_stale_current_and_saved_image_owners);
#endif
		RUN_TEST(relay_paste_preserves_utf8_bytes_and_filters_brackets);
		RUN_TEST(wire_snapshot_aggregate_byte_and_nesting_bounds);
		RUN_TEST(wire_copy_boundary_matches_snapshot_and_ordinary_updates);
		RUN_TEST(wire_copy_outside_boundary_is_atomic_and_keeps_frame_boundaries);
		RUN_TEST(media_snapshot_frozen_mode_uses_its_own_link_table);
		RUN_TEST(media_snapshot_aggregate_cache_bounds_are_explicit);
#ifdef ENABLE_SIXEL
		RUN_TEST(media_snapshot_image_fields_and_pixel_indices_are_bounded);
		RUN_TEST(media_snapshot_canonical_sixel_reserves_exact_buffer_terminator);
#endif
		RUN_TEST(media_snapshot_links_survive_table_replacement_and_alternate_screen);
		RUN_TEST(media_snapshot_rejects_dangling_duplicate_and_control_links);
		RUN_TEST(media_snapshot_detached_links_do_not_evict_live_links);
#ifdef ENABLE_SIXEL
		RUN_TEST(media_snapshot_images_preserve_pixels_palette_and_saved_state);
		RUN_TEST(media_snapshot_rejects_malformed_image_before_install);
#endif
		RUN_TEST(wire_snapshot_media_is_atomic_in_bulk_and_bytewise_transport);
		RUN_TEST(wire_snapshot_installs_all_panes_and_partial_parser);
		RUN_TEST(wire_snapshot_malformed_fields_are_atomic_and_keep_boundaries);
		RUN_TEST(wire_snapshot_metadata_is_safe_and_atomic_in_bulk_and_bytewise_transport);
		RUN_TEST(pane_snapshot_saved_dimensions_survive_alternate_resize);
		RUN_TEST(pane_snapshot_saved_cursor_style_survives_after_alternate_exit);
		RUN_TEST(pane_snapshot_both_grids_share_output_byte_limit);
		RUN_TEST(pane_snapshot_restores_active_saved_screen_and_exit_style);
		RUN_TEST(pane_snapshot_keeps_copy_clock_and_frozen_backing);
		RUN_TEST(pane_snapshot_rejects_identity_and_geometry_before_mutation);
		RUN_TEST(pane_snapshot_rearms_sync_and_replaces_old_write_queue);
		RUN_TEST(pane_snapshot_combined_grid_budget_is_checked_before_allocation);
		RUN_TEST(pane_snapshot_empty_screen_ignores_uninitialized_saved_fields);
		RUN_TEST(pane_snapshot_unsupported_saved_links_and_images_are_atomic);
		RUN_TEST(pane_snapshot_rejects_version);
		RUN_TEST(pane_snapshot_rejects_overflow_id);
		RUN_TEST(pane_snapshot_rejects_cursor_x);
		RUN_TEST(pane_snapshot_rejects_cursor_y);
		RUN_TEST(pane_snapshot_rejects_mode);
		RUN_TEST(pane_snapshot_rejects_default_mode);
		RUN_TEST(pane_snapshot_rejects_cursor_style);
		RUN_TEST(pane_snapshot_rejects_cursor_colour);
		RUN_TEST(pane_snapshot_rejects_scroll_region);
		RUN_TEST(pane_snapshot_rejects_tab_range);
		RUN_TEST(pane_snapshot_rejects_duplicate_tab);
		RUN_TEST(pane_snapshot_rejects_saved_history);
		RUN_TEST(pane_snapshot_rejects_saved_history_flag);
		RUN_TEST(pane_snapshot_rejects_alternate_history_flag);
		RUN_TEST(pane_snapshot_rejects_unpaired_cursor_sentinel);
		RUN_TEST(pane_snapshot_rejects_saved_cursor_range);
		RUN_TEST(pane_snapshot_rejects_missing_saved_style);
		RUN_TEST(pane_snapshot_rejects_saved_colour);
		RUN_TEST(pane_snapshot_rejects_saved_flags);
		RUN_TEST(pane_snapshot_rejects_missing_saved_grid);
		RUN_TEST(pane_snapshot_rejects_saved_grid_rows);
		RUN_TEST(pane_snapshot_rejects_saved_style_length);
		RUN_TEST(utf8_handle_preserves_all_lengths_and_widths);
		RUN_TEST(snapshot_maximum_combined_cell_roundtrips);
		RUN_TEST(snapshot_mutated_objects_remain_bounded_and_reencodable);
		RUN_TEST(snapshot_preserves_host_width_and_full_style_bits);
		RUN_TEST(snapshot_output_limit_is_atomic);
		RUN_TEST(snapshot_history_limit_is_atomic);
		RUN_TEST(snapshot_grid_roundtrip_preserves_text_and_styles);
		RUN_TEST(snapshot_history_is_explicitly_bounded);
		RUN_TEST(snapshot_old_width_rows_survive_resize);
		RUN_TEST(snapshot_cost_bounds_both_encoder_and_decoder);
		RUN_TEST(snapshot_rejected_encode_preserves_destination);
		RUN_TEST(snapshot_rejects_version);
		RUN_TEST(snapshot_rejects_zero_width);
		RUN_TEST(snapshot_rejects_overflow_height);
		RUN_TEST(snapshot_rejects_history_overflow);
		RUN_TEST(snapshot_rejects_scrolled_overflow);
		RUN_TEST(snapshot_rejects_grid_flags);
		RUN_TEST(snapshot_rejects_row_count);
		RUN_TEST(snapshot_rejects_used_overflow);
		RUN_TEST(snapshot_rejects_storage_flags);
		RUN_TEST(snapshot_rejects_osc_overflow);
		RUN_TEST(snapshot_rejects_nonbinary_cell);
		RUN_TEST(snapshot_rejects_oversized_cell);
		RUN_TEST(snapshot_rejects_unsupported_width);
		RUN_TEST(snapshot_rejects_cell_storage_flags);
		RUN_TEST(snapshot_rejects_attribute_overflow);
		RUN_TEST(snapshot_rejects_mixed_colour_types);
		RUN_TEST(snapshot_rejects_negative_colour);
		RUN_TEST(snapshot_rejects_unknown_theme_slot);
		RUN_TEST(snapshot_rejects_missing_padding);
		RUN_TEST(snapshot_rejects_orphan_padding);
		RUN_TEST(snapshot_rejects_overlong_utf8);
		RUN_TEST(snapshot_rejects_surrogate);
		RUN_TEST(snapshot_rejects_out_of_range_scalar);
		RUN_TEST(snapshot_rejects_truncated_utf8);
		RUN_TEST(snapshot_rejects_escape);
		RUN_TEST(snapshot_rejects_c1_control);
		RUN_TEST(snapshot_rejects_del);
		RUN_TEST(snapshot_rejects_nul);
		RUN_TEST(snapshot_rejects_empty_text);
		RUN_TEST(snapshot_rejects_extra_padding);
		RUN_TEST(exact_layout_preserves_floating_state_and_pane_identity);
		RUN_TEST(exact_aliases_create_one_shared_window_and_keep_active_index);
		RUN_TEST(exact_alias_canonical_removal_and_index_reuse_preserve_objects);
		RUN_TEST(exact_invalid_aliases_leave_all_links_and_panes_untouched);
		RUN_TEST(exact_layout_creates_all_panes_before_activation);
		RUN_TEST(exact_layout_moves_panes_between_windows_without_recreating);
		RUN_TEST(exact_layout_preserves_zoom_and_saved_tree);
		RUN_TEST(exact_layout_rejects_invalid_graphs_atomically);
		RUN_TEST(maximum_panes_recover_spatial_tree);
		RUN_TEST(non_tiled_rectangles_preserve_legacy_fallback);
		RUN_TEST(malformed_layout_preserves_session_geometry_and_following_message);
		RUN_TEST(malformed_snapshot_preserves_both_grids_and_message_boundaries);
		RUN_TEST(web_snapshot_fields_and_history);
		RUN_TEST(fragmented_web_snapshot_fields_without_history);
		RUN_TEST(empty_session_web_snapshot_is_complete);
		RUN_TEST(initial_and_repeated_output);
		RUN_TEST(fragmented_v4_message);
		RUN_TEST(unknown_pane_is_discarded);
		RUN_TEST(invalid_output_id_cannot_enter_mode);
		RUN_TEST(invalid_output_id_preserves_stack_and_backing);
		RUN_TEST(active_clock_mode_is_preserved_under_output);
		RUN_TEST(active_copy_mode_has_separate_backing);
		RUN_TEST(existing_view_mode_is_resumed);
		RUN_TEST(empty_state_exits_copy_and_preserves_unrelated_modes);
		RUN_TEST(nonempty_and_invalid_pane_state_do_not_exit);
		RUN_TEST(valid_copy_state_restores_cursor_selection_and_stack);
		RUN_TEST(fragmented_valid_copy_state_restores);
		RUN_TEST(malformed_copy_state_is_atomic_and_keeps_boundaries);
		RUN_TEST(view_state_uses_independent_backing_history);
	}
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	TEST_SUITE_END();
}
