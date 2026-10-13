/* Complete relay production objects: real streaming decoder and mode lifecycle. */
#include "../server/tmate.h"
#include "../server/tmate-protocol.h"
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
	grid_set_padding(wp->base.grid, 2, 2);
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
	ASSERT_EQ(dispatched, before + (host_exit ? 4 : 3));
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
	if (argc == 2 && (strcmp(argv[1], "--host-snapshot") == 0 ||
	    strcmp(argv[1], "--host-snapshot-bulk") == 0)) {
		snapshot_bulk = strcmp(argv[1], "--host-snapshot-bulk") == 0;
		RUN_TEST(host_snapshot_restores_grids_without_mutating_mode_backing);
	} else if (argc == 2 && (strcmp(argv[1], "--host-output") == 0 ||
	    strcmp(argv[1], "--host-exit") == 0)) {
		host_exit = strcmp(argv[1], "--host-exit") == 0;
		RUN_TEST(host_encoder_output_routes_without_feedback);
	} else {
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
	}
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	TEST_SUITE_END();
}
