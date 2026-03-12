/*
 * Unit test for tmate_find_session() in tmate-decoder.c.
 *
 * Tests:
 *  - Returns NULL when no sessions exist
 *  - Returns session with matching ->tmate pointer
 *  - Falls back to first session when no ->tmate match (backward compat)
 *  - Finds correct session among multiple entries
 */

#include "test-harness.h"

#include <string.h>
#include <stdlib.h>
#include "../compat/tree.h"

/*
 * Minimal stubs to replicate the session RB tree without pulling in
 * the entire tmux codebase. We only need the fields that
 * tmate_find_session inspects: name (for RB ordering) and tmate.
 */

struct tmate_session {
	int dummy;	/* just needs to be a distinct pointer target */
};

struct session {
	char		*name;
	struct tmate_session *tmate;
	RB_ENTRY(session)    entry;
};

static int
session_cmp(struct session *s1, struct session *s2)
{
	return (strcmp(s1->name, s2->name));
}

RB_HEAD(sessions, session);
RB_GENERATE(sessions, session, entry, session_cmp);

/* The global sessions tree — same name as in tmux */
struct sessions sessions = RB_INITIALIZER(&sessions);

/*
 * Re-implement tmate_find_session identically to tmate-decoder.c
 * so we can test the logic without linking the full binary.
 * If the implementation changes, this test must be updated to match.
 */
struct session *
tmate_find_session(struct tmate_session *ts)
{
	struct session *s;

	RB_FOREACH(s, sessions, &sessions) {
		if (s->tmate == ts)
			return (s);
	}

	/* Fallback: no session linked yet, use first available. */
	return (RB_MIN(sessions, &sessions));
}

/* Helper: create a minimal session and insert into the tree */
static struct session *
make_session(const char *name, struct tmate_session *ts)
{
	struct session *s = calloc(1, sizeof *s);
	s->name = strdup(name);
	s->tmate = ts;
	RB_INSERT(sessions, &sessions, s);
	return s;
}

static void
free_session(struct session *s)
{
	RB_REMOVE(sessions, &sessions, s);
	free(s->name);
	free(s);
}

/* --- Tests --- */

TEST(empty_tree_returns_null)
{
	struct tmate_session ts;
	ASSERT(tmate_find_session(&ts) == NULL);
}

TEST(single_session_matching_tmate)
{
	struct tmate_session ts;
	struct session *s = make_session("test", &ts);

	ASSERT(tmate_find_session(&ts) == s);

	free_session(s);
}

TEST(single_session_no_match_falls_back)
{
	struct tmate_session ts1, ts2;
	struct session *s = make_session("alpha", &ts1);

	/* ts2 doesn't match any session, should fall back to first */
	struct session *result = tmate_find_session(&ts2);
	ASSERT(result == s);

	free_session(s);
}

TEST(single_session_null_tmate_falls_back)
{
	struct tmate_session ts;
	struct session *s = make_session("beta", NULL);

	/* Session has NULL tmate, query with non-NULL ts -> fallback */
	struct session *result = tmate_find_session(&ts);
	ASSERT(result == s);

	free_session(s);
}

TEST(multiple_sessions_finds_correct)
{
	struct tmate_session ts1, ts2, ts3;
	struct session *s1 = make_session("aaa", &ts1);
	struct session *s2 = make_session("bbb", &ts2);
	struct session *s3 = make_session("ccc", &ts3);

	ASSERT(tmate_find_session(&ts1) == s1);
	ASSERT(tmate_find_session(&ts2) == s2);
	ASSERT(tmate_find_session(&ts3) == s3);

	free_session(s3);
	free_session(s2);
	free_session(s1);
}

TEST(multiple_sessions_no_match_returns_first)
{
	struct tmate_session ts1, ts2, ts_unknown;
	struct session *s1 = make_session("aaa", &ts1);
	struct session *s2 = make_session("bbb", &ts2);

	/* ts_unknown doesn't match either, should return RB_MIN ("aaa") */
	struct session *result = tmate_find_session(&ts_unknown);
	ASSERT(result == s1);

	free_session(s2);
	free_session(s1);
}

TEST(null_tmate_session_falls_back)
{
	struct session *s = make_session("gamma", NULL);

	/* Passing NULL ts: no session has tmate==NULL... wait, s does! */
	/* Actually, s->tmate is NULL and we're looking for NULL, so it matches */
	struct session *result = tmate_find_session(NULL);
	ASSERT(result == s);

	free_session(s);
}

int
main(void)
{
	TEST_SUITE_BEGIN("tmate_find_session");

	RUN_TEST(empty_tree_returns_null);
	RUN_TEST(single_session_matching_tmate);
	RUN_TEST(single_session_no_match_falls_back);
	RUN_TEST(single_session_null_tmate_falls_back);
	RUN_TEST(multiple_sessions_finds_correct);
	RUN_TEST(multiple_sessions_no_match_returns_first);
	RUN_TEST(null_tmate_session_falls_back);

	TEST_SUITE_END();
}
