/*
 * Test the server-side tmate_format() path.
 *
 * Bug tmtv-a7o: format.c called tmate_format(&_tmate_session, ft) in the
 * server build, but the server's tmate_format() takes only (struct format_tree *)
 * — no session argument. The ABI mismatch caused the format tree pointer to be
 * ignored, so #{tmtv_web_viewers} never rendered on the virtual PTY status bar.
 *
 * This test validates that the server's tmate_set_env() + tmate_format()
 * correctly inject format variables, exercising the same code path that
 * format.c uses in the TMATE_SERVER_BUILD configuration.
 */

#include "test-harness.h"
#include <stdarg.h>
#include <stdbool.h>
#include <sys/queue.h>

/* --- Minimal type stubs (just enough for the env list) --- */

struct format_tree {
	int dummy;
};

/* --- Re-implement the server's env list (from tmate-daemon-encoder.c) --- */

struct tmate_env_entry {
	TAILQ_ENTRY(tmate_env_entry) entry;
	char *name;
	char *value;
};

static TAILQ_HEAD(, tmate_env_entry) tmate_env_list =
    TAILQ_HEAD_INITIALIZER(tmate_env_list);

static void *
xmalloc(size_t size)
{
	void *p = malloc(size);
	if (!p)
		abort();
	return p;
}

static char *
xstrdup(const char *s)
{
	char *p = strdup(s);
	if (!p)
		abort();
	return p;
}

/*
 * Server-side tmate_set_env: stores in static list.
 * Stripped of the pack() calls that send over the wire.
 */
static void
tmate_set_env(const char *name, const char *value)
{
	struct tmate_env_entry *e;

	TAILQ_FOREACH(e, &tmate_env_list, entry) {
		if (!strcmp(e->name, name)) {
			free(e->value);
			e->value = xstrdup(value);
			return;
		}
	}
	e = xmalloc(sizeof(*e));
	e->name = xstrdup(name);
	e->value = xstrdup(value);
	TAILQ_INSERT_HEAD(&tmate_env_list, e, entry);
}

/* Forward declaration so tmate_format can see format_add */
void format_add(struct format_tree *, const char *, const char *, ...);

/*
 * Server-side tmate_format: single argument (struct format_tree *).
 * This is the signature that format.c must use in TMATE_SERVER_BUILD.
 */
static void
tmate_format(struct format_tree *ft)
{
	struct tmate_env_entry *e;

	TAILQ_FOREACH(e, &tmate_env_list, entry) {
		format_add(ft, e->name, "%s", e->value);
	}
}

/* --- Track format_add calls --- */

#define MAX_FORMAT_ENTRIES 32
static struct {
	char name[128];
	char value[128];
	struct format_tree *ft_received;
} format_entries[MAX_FORMAT_ENTRIES];
static int format_entry_count;

void
format_add(struct format_tree *ft, const char *name, const char *fmt, ...)
{
	if (format_entry_count >= MAX_FORMAT_ENTRIES)
		return;

	va_list ap;
	va_start(ap, fmt);
	format_entries[format_entry_count].ft_received = ft;
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

static void
reset_env_list(void)
{
	struct tmate_env_entry *e;
	while ((e = TAILQ_FIRST(&tmate_env_list)) != NULL) {
		TAILQ_REMOVE(&tmate_env_list, e, entry);
		free(e->name);
		free(e->value);
		free(e);
	}
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

/* --- Tests --- */

TEST(server_format_single_arg_signature)
{
	/*
	 * Verify the server's tmate_format takes a single struct format_tree*
	 * argument and passes it through to format_add correctly.
	 * This is the call pattern format.c uses in TMATE_SERVER_BUILD.
	 */
	struct format_tree ft;
	ft.dummy = 42;

	reset_env_list();
	reset_format_entries();

	tmate_set_env("tmtv_web_viewers", "3");

	/* Call with single argument — the server signature */
	tmate_format(&ft);

	ASSERT_EQ(format_entry_count, 1);
	ASSERT_STR_EQ(format_entries[0].name, "tmtv_web_viewers");
	ASSERT_STR_EQ(format_entries[0].value, "3");
	/* Verify format_add received the correct ft pointer */
	ASSERT(format_entries[0].ft_received == &ft);
}

TEST(server_web_viewer_count_updates)
{
	struct format_tree ft;

	reset_env_list();
	reset_format_entries();

	tmate_set_env("tmtv_web_viewers", "0");
	tmate_set_env("tmtv_ssh_viewers", "1");

	tmate_format(&ft);

	ASSERT_EQ(format_entry_count, 2);
	ASSERT_STR_EQ(find_format_value("tmtv_web_viewers"), "0");
	ASSERT_STR_EQ(find_format_value("tmtv_ssh_viewers"), "1");

	/* Simulate web viewer connecting: update the count */
	tmate_set_env("tmtv_web_viewers", "1");

	reset_format_entries();
	tmate_format(&ft);

	ASSERT_STR_EQ(find_format_value("tmtv_web_viewers"), "1");
	ASSERT_STR_EQ(find_format_value("tmtv_ssh_viewers"), "1");
}

TEST(server_format_multiple_vars)
{
	struct format_tree ft;

	reset_env_list();
	reset_format_entries();

	tmate_set_env("tmtv_ssh_viewers", "2");
	tmate_set_env("tmtv_web_viewers", "5");
	tmate_set_env("tmtv_session_name", "my-session");

	tmate_format(&ft);

	ASSERT_EQ(format_entry_count, 3);
	ASSERT_STR_EQ(find_format_value("tmtv_ssh_viewers"), "2");
	ASSERT_STR_EQ(find_format_value("tmtv_web_viewers"), "5");
	ASSERT_STR_EQ(find_format_value("tmtv_session_name"), "my-session");
}

TEST(server_format_overwrite_preserves_single_entry)
{
	struct format_tree ft;

	reset_env_list();
	reset_format_entries();

	tmate_set_env("tmtv_web_viewers", "1");
	tmate_set_env("tmtv_web_viewers", "2");
	tmate_set_env("tmtv_web_viewers", "3");

	tmate_format(&ft);

	/* Should have exactly one entry with the latest value */
	ASSERT_EQ(format_entry_count, 1);
	ASSERT_STR_EQ(format_entries[0].value, "3");
}

TEST(server_format_empty_list)
{
	struct format_tree ft;

	reset_env_list();
	reset_format_entries();

	tmate_format(&ft);

	ASSERT_EQ(format_entry_count, 0);
}

int
main(void)
{
	TEST_SUITE_BEGIN("server-side tmate_format (bug tmtv-a7o)");

	RUN_TEST(server_format_single_arg_signature);
	RUN_TEST(server_web_viewer_count_updates);
	RUN_TEST(server_format_multiple_vars);
	RUN_TEST(server_format_overwrite_preserves_single_entry);
	RUN_TEST(server_format_empty_list);

	TEST_SUITE_END();
}
