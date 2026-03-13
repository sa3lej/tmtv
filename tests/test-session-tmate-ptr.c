/*
 * Test that struct session has a tmate pointer (multi-session foundation).
 *
 * Verifies:
 * 1. struct session has a 'tmate' field (compile-time)
 * 2. The field is a pointer to struct tmate_session
 * 3. xcalloc zeroes it to NULL
 * 4. Assignment works correctly
 */

#include "test-harness.h"
#include "../tmate.h"

/* Verify struct session has a tmate pointer field */
TEST(session_has_tmate_ptr)
{
	struct session s;
	memset(&s, 0, sizeof(s));

	/* Field exists and is NULL after zero-init */
	ASSERT(s.tmate == NULL);
}

/* Verify the pointer can be assigned to a tmate_session */
TEST(session_tmate_ptr_assignment)
{
	struct session s;
	struct tmate_session ts;

	memset(&s, 0, sizeof(s));
	memset(&ts, 0, sizeof(ts));

	s.tmate = &ts;
	ASSERT(s.tmate == &ts);
	ASSERT(s.tmate->min_sx == 0);
}

/* Verify the global tmate_session extern is accessible */
TEST(global_tmate_session_accessible)
{
	/*
	 * The extern declaration in tmate.h should make
	 * tmate_session available. We can take its address.
	 */
	struct tmate_session *p = &tmate_session;
	ASSERT(p != NULL);
}

/* Verify pointer assignment to global works (the compatibility shim) */
TEST(session_points_to_global)
{
	struct session s;
	memset(&s, 0, sizeof(s));

	s.tmate = &tmate_session;
	ASSERT(s.tmate == &tmate_session);
}

/*
 * We need a minimal definition of the global so the test links.
 * In real code, tmate-session.c provides this.
 */
struct tmate_session tmate_session;
int tmate_foreground;

int main(void)
{
	TEST_SUITE_BEGIN("struct session tmate pointer (multi-session foundation)");

	RUN_TEST(session_has_tmate_ptr);
	RUN_TEST(session_tmate_ptr_assignment);
	RUN_TEST(global_tmate_session_accessible);
	RUN_TEST(session_points_to_global);

	TEST_SUITE_END();
}
