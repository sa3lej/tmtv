/*
 * Test the pty_pending buffer-and-replay system.
 *
 * The buffer stores pty_data for panes that don't exist yet (race between
 * pty_data and sync_windows), then replays it when the pane is created.
 *
 * We extract the buffer logic by including the relevant parts and stubbing
 * the tmux functions (window_pane_find_by_id, input_parse_buffer, etc.).
 */

#include "test-harness.h"
#include <sys/queue.h>
#include <stdint.h>

/* Some systems lack TAILQ_FOREACH_SAFE */
#ifndef TAILQ_FOREACH_SAFE
#define TAILQ_FOREACH_SAFE(var, head, field, tvar) \
	for ((var) = TAILQ_FIRST((head)); \
	     (var) && ((tvar) = TAILQ_NEXT((var), field), 1); \
	     (var) = (tvar))
#endif

/* --- Stubs for tmux types and functions --- */

typedef unsigned int u_int;
typedef unsigned char u_char;

struct window {
	unsigned int flags;
};

#define WINDOW_SILENCE 0x1

struct window_pane {
	int id;
	struct window *window;
	u_char *replay_buf;    /* capture what input_parse_buffer receives */
	size_t  replay_len;
};

/* Simulated pane registry */
#define MAX_TEST_PANES 8
static struct window_pane *test_panes[MAX_TEST_PANES];
static int test_pane_count;
static struct window test_window;

static struct window_pane *window_pane_find_by_id(int id)
{
	int i;
	for (i = 0; i < test_pane_count; i++) {
		if (test_panes[i]->id == id)
			return test_panes[i];
	}
	return NULL;
}

static void input_parse_buffer(struct window_pane *wp, u_char *data,
			       size_t len)
{
	/* Append to the pane's replay buffer for verification */
	wp->replay_buf = realloc(wp->replay_buf, wp->replay_len + len);
	memcpy(wp->replay_buf + wp->replay_len, data, len);
	wp->replay_len += len;
}

static struct window_pane *add_test_pane(int id)
{
	struct window_pane *wp = calloc(1, sizeof(*wp));
	wp->id = id;
	wp->window = &test_window;
	test_panes[test_pane_count++] = wp;
	return wp;
}

static void reset_test_panes(void)
{
	int i;
	for (i = 0; i < test_pane_count; i++) {
		free(test_panes[i]->replay_buf);
		free(test_panes[i]);
	}
	test_pane_count = 0;
	memset(&test_window, 0, sizeof(test_window));
}

/* Stubs for logging */
#define tmate_info(...) do { } while (0)

/* Stubs for allocation (use real malloc) */
#define xcalloc(n, sz) calloc((n), (sz))
#define xrealloc(p, sz) realloc((p), (sz))

/* --- Include the actual buffer implementation --- */

/* We replicate the structs and functions here since they use statics */

#define PTY_PENDING_MAX       (128 * 1024)
#define PTY_PENDING_TOTAL_MAX (2 * 1024 * 1024)

struct pty_pending {
	TAILQ_ENTRY(pty_pending) entry;
	int     id;
	u_char *data;
	size_t  len;
	size_t  cap;
	int     count;
};

static TAILQ_HEAD(, pty_pending) pty_pending_list =
    TAILQ_HEAD_INITIALIZER(pty_pending_list);
static size_t pty_pending_total;

static void pty_pending_free(struct pty_pending *p)
{
	TAILQ_REMOVE(&pty_pending_list, p, entry);
	pty_pending_total -= p->len;
	free(p->data);
	free(p);
}

static void pty_pending_replay(int pane_id)
{
	struct pty_pending *p, *tmp;
	struct window_pane *wp;

	TAILQ_FOREACH_SAFE(p, &pty_pending_list, entry, tmp) {
		if (p->id != pane_id)
			continue;
		wp = window_pane_find_by_id(pane_id);
		if (wp && p->len > 0) {
			input_parse_buffer(wp, p->data, p->len);
			wp->window->flags |= WINDOW_SILENCE;
		}
		pty_pending_free(p);
		return;
	}
}

static void pty_pending_flush_stale(void)
{
	struct pty_pending *p, *tmp;

	TAILQ_FOREACH_SAFE(p, &pty_pending_list, entry, tmp) {
		if (window_pane_find_by_id(p->id))
			continue;
		pty_pending_free(p);
	}
}

/* Simulate tmate_pty_data's buffering logic */
static void buffer_pty_data(int id, const void *buf, size_t len)
{
	struct window_pane *wp;
	struct pty_pending *p;

	wp = window_pane_find_by_id(id);
	if (wp) {
		input_parse_buffer(wp, (u_char *)buf, len);
		wp->window->flags |= WINDOW_SILENCE;
		return;
	}

	TAILQ_FOREACH(p, &pty_pending_list, entry) {
		if (p->id == id)
			break;
	}

	if (!p) {
		while (pty_pending_total + len > PTY_PENDING_TOTAL_MAX
		       && !TAILQ_EMPTY(&pty_pending_list)) {
			struct pty_pending *oldest =
			    TAILQ_FIRST(&pty_pending_list);
			pty_pending_free(oldest);
		}

		p = xcalloc(1, sizeof(*p));
		p->id = id;
		TAILQ_INSERT_TAIL(&pty_pending_list, p, entry);
	}

	if (p->len + len <= PTY_PENDING_MAX) {
		if (p->len + len > p->cap) {
			size_t newcap = p->cap ? p->cap * 2 : 8192;
			if (newcap < p->len + len)
				newcap = p->len + len;
			if (newcap > PTY_PENDING_MAX)
				newcap = PTY_PENDING_MAX;
			p->data = xrealloc(p->data, newcap);
			p->cap = newcap;
		}
		memcpy(p->data + p->len, buf, len);
		p->len += len;
		pty_pending_total += len;
	}

	p->count++;
}

static void reset_pending(void)
{
	struct pty_pending *p, *tmp;
	TAILQ_FOREACH_SAFE(p, &pty_pending_list, entry, tmp)
		pty_pending_free(p);
	pty_pending_total = 0;
}

/* --- Tests --- */

TEST(buffer_and_replay_single_pane)
{
	reset_pending();
	reset_test_panes();

	/* Send data before pane exists */
	buffer_pty_data(5, "hello", 5);
	buffer_pty_data(5, " world", 6);

	/* Pane doesn't exist yet — nothing replayed */
	ASSERT(window_pane_find_by_id(5) == NULL);

	/* Create the pane and replay */
	struct window_pane *wp = add_test_pane(5);
	pty_pending_replay(5);

	/* Verify data was replayed in order */
	ASSERT_EQ(wp->replay_len, 11);
	ASSERT(memcmp(wp->replay_buf, "hello world", 11) == 0);

	/* Pending list should be empty */
	ASSERT(TAILQ_EMPTY(&pty_pending_list));
	ASSERT_EQ(pty_pending_total, 0);
}

TEST(buffer_multiple_panes)
{
	reset_pending();
	reset_test_panes();

	buffer_pty_data(10, "pane10", 6);
	buffer_pty_data(11, "pane11", 6);
	buffer_pty_data(12, "pane12", 6);
	buffer_pty_data(10, "-more", 5);

	/* Create pane 11 first, replay it */
	struct window_pane *wp11 = add_test_pane(11);
	pty_pending_replay(11);

	ASSERT_EQ(wp11->replay_len, 6);
	ASSERT(memcmp(wp11->replay_buf, "pane11", 6) == 0);

	/* Panes 10 and 12 still pending */
	struct pty_pending *p;
	int pending_count = 0;
	TAILQ_FOREACH(p, &pty_pending_list, entry)
		pending_count++;
	ASSERT_EQ(pending_count, 2);

	/* Create pane 10, replay it */
	struct window_pane *wp10 = add_test_pane(10);
	pty_pending_replay(10);

	ASSERT_EQ(wp10->replay_len, 11);
	ASSERT(memcmp(wp10->replay_buf, "pane10-more", 11) == 0);

	/* Create pane 12, replay it */
	struct window_pane *wp12 = add_test_pane(12);
	pty_pending_replay(12);

	ASSERT_EQ(wp12->replay_len, 6);
	ASSERT(memcmp(wp12->replay_buf, "pane12", 6) == 0);

	ASSERT(TAILQ_EMPTY(&pty_pending_list));
	ASSERT_EQ(pty_pending_total, 0);
}

TEST(flush_stale_discards_unknown)
{
	reset_pending();
	reset_test_panes();

	buffer_pty_data(20, "stale1", 6);
	buffer_pty_data(21, "stale2", 6);

	/* Neither pane exists — flush should discard both */
	pty_pending_flush_stale();

	ASSERT(TAILQ_EMPTY(&pty_pending_list));
	ASSERT_EQ(pty_pending_total, 0);
}

TEST(flush_stale_keeps_existing)
{
	reset_pending();
	reset_test_panes();

	buffer_pty_data(30, "keep", 4);
	buffer_pty_data(31, "discard", 7);

	/* Create pane 30 — it exists now */
	add_test_pane(30);

	/* Flush stale — should keep 30, discard 31 */
	pty_pending_flush_stale();

	int pending_count = 0;
	struct pty_pending *p;
	TAILQ_FOREACH(p, &pty_pending_list, entry)
		pending_count++;
	ASSERT_EQ(pending_count, 1);

	/* Replay pane 30 */
	struct window_pane *wp = window_pane_find_by_id(30);
	pty_pending_replay(30);

	ASSERT_EQ(wp->replay_len, 4);
	ASSERT(memcmp(wp->replay_buf, "keep", 4) == 0);
}

TEST(per_pane_cap_enforced)
{
	reset_pending();
	reset_test_panes();

	/* Fill a pane to its cap */
	char bigbuf[8192];
	memset(bigbuf, 'A', sizeof(bigbuf));

	size_t total = 0;
	while (total + sizeof(bigbuf) <= PTY_PENDING_MAX) {
		buffer_pty_data(40, bigbuf, sizeof(bigbuf));
		total += sizeof(bigbuf);
	}

	/* One more chunk that would exceed the cap */
	buffer_pty_data(40, bigbuf, sizeof(bigbuf));

	/* Buffer should be at cap, not over */
	struct pty_pending *p;
	TAILQ_FOREACH(p, &pty_pending_list, entry) {
		if (p->id == 40)
			break;
	}
	ASSERT_NOT_NULL(p);
	ASSERT(p->len <= PTY_PENDING_MAX);
	ASSERT_EQ(p->len, total); /* last chunk was dropped */

	reset_pending();
}

TEST(total_cap_evicts_oldest)
{
	reset_pending();
	reset_test_panes();

	/* Fill with data for many panes to approach total cap */
	char buf[PTY_PENDING_MAX];
	memset(buf, 'B', sizeof(buf));

	/* 2MB / 128KB = 16 panes at full cap */
	int i;
	for (i = 0; i < 16; i++)
		buffer_pty_data(100 + i, buf, PTY_PENDING_MAX);

	ASSERT(pty_pending_total <= PTY_PENDING_TOTAL_MAX);

	/* Adding one more should evict the oldest (pane 100) */
	buffer_pty_data(200, "new", 3);

	/* Pane 100 should be gone */
	struct pty_pending *p;
	TAILQ_FOREACH(p, &pty_pending_list, entry) {
		ASSERT(p->id != 100);
	}

	/* New pane should exist */
	int found = 0;
	TAILQ_FOREACH(p, &pty_pending_list, entry) {
		if (p->id == 200) {
			found = 1;
			ASSERT_EQ(p->len, 3);
		}
	}
	ASSERT(found);

	reset_pending();
}

TEST(direct_delivery_when_pane_exists)
{
	reset_pending();
	reset_test_panes();

	struct window_pane *wp = add_test_pane(50);

	/* Data should go directly to pane, not buffer */
	buffer_pty_data(50, "direct", 6);

	ASSERT_EQ(wp->replay_len, 6);
	ASSERT(memcmp(wp->replay_buf, "direct", 6) == 0);
	ASSERT(TAILQ_EMPTY(&pty_pending_list));
}

TEST(replay_for_nonexistent_pane_is_noop)
{
	reset_pending();
	reset_test_panes();

	/* Replay for a pane with no pending data — should not crash */
	pty_pending_replay(999);

	ASSERT(TAILQ_EMPTY(&pty_pending_list));
}

TEST(replay_clears_buffer)
{
	reset_pending();
	reset_test_panes();

	buffer_pty_data(60, "first", 5);

	struct window_pane *wp = add_test_pane(60);
	pty_pending_replay(60);

	ASSERT_EQ(wp->replay_len, 5);

	/* Second replay should be a no-op (buffer was freed) */
	pty_pending_replay(60);
	ASSERT_EQ(wp->replay_len, 5); /* unchanged */
}

int main(void)
{
	TEST_SUITE_BEGIN("pty_pending buffer-and-replay");

	RUN_TEST(buffer_and_replay_single_pane);
	RUN_TEST(buffer_multiple_panes);
	RUN_TEST(flush_stale_discards_unknown);
	RUN_TEST(flush_stale_keeps_existing);
	RUN_TEST(per_pane_cap_enforced);
	RUN_TEST(total_cap_evicts_oldest);
	RUN_TEST(direct_delivery_when_pane_exists);
	RUN_TEST(replay_for_nonexistent_pane_is_noop);
	RUN_TEST(replay_clears_buffer);

	TEST_SUITE_END();
}
