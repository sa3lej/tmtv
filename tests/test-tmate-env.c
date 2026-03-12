/*
 * Test that tmate_env_list is per-session (TAILQ in struct tmate_session)
 * rather than a global list. Exercises tmate_set_env() and tmate_format()
 * through two independent session structs.
 *
 * We pull in tmate-env.c via -include to get the implementation without
 * linking the full tmtv binary. We stub xmalloc/xstrdup/format_add.
 */

#include "test-harness.h"

/*
 * tmux.h and tmate.h are already included via -include ../tmate-env.c,
 * which includes tmate.h, which includes tmux.h. We just need our stubs
 * and the test harness.
 */

/* Stubs for functions called by tmate-env.c */
void *
xmalloc(size_t size)
{
	void *p = malloc(size);
	if (!p) {
		fprintf(stderr, "xmalloc: out of memory\n");
		abort();
	}
	return p;
}

char *
xstrdup(const char *s)
{
	char *p = strdup(s);
	if (!p) {
		fprintf(stderr, "xstrdup: out of memory\n");
		abort();
	}
	return p;
}

/* Track format_add calls for verification */
#define MAX_FORMAT_ENTRIES 32
static struct {
	char name[128];
	char value[128];
} format_entries[MAX_FORMAT_ENTRIES];
static int format_entry_count;

void
format_add(struct format_tree *ft __attribute__((unused)),
	   const char *name, const char *fmt, ...)
{
	if (format_entry_count >= MAX_FORMAT_ENTRIES)
		return;

	va_list ap;
	va_start(ap, fmt);
	strlcpy(format_entries[format_entry_count].name, name,
		sizeof(format_entries[0].name));
	vsnprintf(format_entries[format_entry_count].value,
		  sizeof(format_entries[0].value), fmt, ap);
	va_end(ap);
	format_entry_count++;
}

static void
reset_format_entries(void)
{
	format_entry_count = 0;
	memset(format_entries, 0, sizeof(format_entries));
}

static const char *
find_format_value(const char *name)
{
	for (int i = 0; i < format_entry_count; i++) {
		if (strcmp(format_entries[i].name, name) == 0)
			return format_entries[i].value;
	}
	return NULL;
}

/* Helper: initialize just the env list of a session */
static void
init_session_env(struct tmate_session *s)
{
	memset(s, 0, sizeof(*s));
	TAILQ_INIT(&s->env);
}

/* --- Tests --- */

TEST(set_and_retrieve_single)
{
	struct tmate_session s;
	init_session_env(&s);

	tmate_set_env(&s, "tmtv_ssh", "ssh://example.com/abc");

	reset_format_entries();
	tmate_format(&s, NULL);

	ASSERT_EQ(format_entry_count, 1);
	ASSERT_STR_EQ(format_entries[0].name, "tmtv_ssh");
	ASSERT_STR_EQ(format_entries[0].value, "ssh://example.com/abc");
}

TEST(overwrite_existing_key)
{
	struct tmate_session s;
	init_session_env(&s);

	tmate_set_env(&s, "tmtv_ssh", "old_value");
	tmate_set_env(&s, "tmtv_ssh", "new_value");

	reset_format_entries();
	tmate_format(&s, NULL);

	/* Should have exactly one entry, with the updated value */
	ASSERT_EQ(format_entry_count, 1);
	ASSERT_STR_EQ(format_entries[0].value, "new_value");
}

TEST(multiple_keys)
{
	struct tmate_session s;
	init_session_env(&s);

	tmate_set_env(&s, "tmtv_ssh", "ssh://host/abc");
	tmate_set_env(&s, "tmtv_web", "https://host/abc");
	tmate_set_env(&s, "tmtv_ssh_ro", "ssh://host/def");

	reset_format_entries();
	tmate_format(&s, NULL);

	ASSERT_EQ(format_entry_count, 3);
	ASSERT_NOT_NULL(find_format_value("tmtv_ssh"));
	ASSERT_NOT_NULL(find_format_value("tmtv_web"));
	ASSERT_NOT_NULL(find_format_value("tmtv_ssh_ro"));
	ASSERT_STR_EQ(find_format_value("tmtv_ssh"), "ssh://host/abc");
	ASSERT_STR_EQ(find_format_value("tmtv_web"), "https://host/abc");
}

TEST(two_sessions_are_independent)
{
	struct tmate_session s1, s2;
	init_session_env(&s1);
	init_session_env(&s2);

	tmate_set_env(&s1, "tmtv_ssh", "session1_ssh");
	tmate_set_env(&s1, "tmtv_web", "session1_web");

	tmate_set_env(&s2, "tmtv_ssh", "session2_ssh");
	tmate_set_env(&s2, "tmtv_name", "session2_name");

	/* Verify session 1 */
	reset_format_entries();
	tmate_format(&s1, NULL);
	ASSERT_EQ(format_entry_count, 2);
	ASSERT_STR_EQ(find_format_value("tmtv_ssh"), "session1_ssh");
	ASSERT_STR_EQ(find_format_value("tmtv_web"), "session1_web");
	/* s1 must NOT have s2's tmtv_name */
	ASSERT(find_format_value("tmtv_name") == NULL);

	/* Verify session 2 */
	reset_format_entries();
	tmate_format(&s2, NULL);
	ASSERT_EQ(format_entry_count, 2);
	ASSERT_STR_EQ(find_format_value("tmtv_ssh"), "session2_ssh");
	ASSERT_STR_EQ(find_format_value("tmtv_name"), "session2_name");
	/* s2 must NOT have s1's tmtv_web */
	ASSERT(find_format_value("tmtv_web") == NULL);
}

TEST(empty_session_produces_no_format_entries)
{
	struct tmate_session s;
	init_session_env(&s);

	reset_format_entries();
	tmate_format(&s, NULL);

	ASSERT_EQ(format_entry_count, 0);
}

int
main(void)
{
	TEST_SUITE_BEGIN("tmate_env per-session");

	RUN_TEST(set_and_retrieve_single);
	RUN_TEST(overwrite_existing_key);
	RUN_TEST(multiple_keys);
	RUN_TEST(two_sessions_are_independent);
	RUN_TEST(empty_session_produces_no_format_entries);

	TEST_SUITE_END();
}
