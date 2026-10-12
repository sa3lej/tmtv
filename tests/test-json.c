/* Exercise the production parser linked into both host and relay. */
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include "tmux.h"
#include "test-harness.h"

/* Allocation failure remains fatal in production; give child tests a stable exit. */
void fatalx(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	_exit(77);
}
void fatal(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	_exit(78);
}

TEST(layout_shape_roundtrip)
{
	const char *input = "{\"panes\":[{\"id\":1,\"visible\":true},{\"id\":2,\"visible\":false}],\"name\":\"floating\\nlayout\",\"min\":-9223372036854775808,\"max\":9223372036854775807}";
	struct json_node *root, *array, *pane, *copy;
	char *cause = NULL, *serialized;
	const char *name;
	int64_t number;
	int visible;

	root = json_parse(input, &cause);
	ASSERT_NOT_NULL(root);
	ASSERT(cause == NULL);
	ASSERT_EQ(json_find_array(root, "panes", &array, &cause), 0);
	pane = json_array_first(array);
	ASSERT_NOT_NULL(pane);
	ASSERT_EQ(json_find_number(pane, "id", &number, &cause), 0);
	ASSERT_EQ(number, 1);
	ASSERT_EQ(json_find_boolean(pane, "visible", &visible, &cause), 0);
	ASSERT_EQ(visible, 1);
	pane = json_array_next(pane);
	ASSERT_NOT_NULL(pane);
	ASSERT_EQ(json_find_number(pane, "id", &number, &cause), 0);
	ASSERT_EQ(number, 2);
	ASSERT_EQ(json_find_boolean(pane, "visible", &visible, &cause), 0);
	ASSERT_EQ(visible, 0);
	ASSERT(json_array_next(pane) == NULL);
	ASSERT_EQ(json_find_string(root, "name", &name, &cause), 0);
	/* Upstream validates escapes and preserves them without decoding. */
	ASSERT_STR_EQ(name, "floating\\nlayout");
	ASSERT_EQ(json_find_number(root, "min", &number, &cause), 0);
	ASSERT_EQ(number, INT64_MIN);
	ASSERT_EQ(json_find_number(root, "max", &number, &cause), 0);
	ASSERT_EQ(number, INT64_MAX);
	serialized = json_to_string(root);
	ASSERT_NOT_NULL(serialized);
	copy = json_parse(serialized, &cause);
	ASSERT_NOT_NULL(copy);
	ASSERT_EQ(json_find_string(copy, "name", &name, &cause), 0);
	ASSERT_STR_EQ(name, "floating\\nlayout");
	json_destroy_node(copy);
	json_destroy_node(root);
	free(serialized);
}

TEST(rejects_malformed_and_unsupported_inputs)
{
	const char *bad[] = {
	    "", " ", "[]", "{} trailing", "{", "{\"x\":1,}",
	    "{\"x\":1,\"x\":2}", "{\"x\":9223372036854775808}",
	    "{\"x\":-9223372036854775809}", "{\"x\":01}",
	    "{\"x\":1.5}", "{\"x\":1e2}", "{\"x\":null}",
	    "{\"x\":\"\"}", "{\"x\":\"\\q\"}", "{\"x\":\"\\u12\"}",
	    "{\"x\":\"unterminated}", "{\"x\":[1]}", "{\"x\":[{},]}"
	};
	size_t i;
	char *cause;
	struct json_node *root;

	for (i = 0; i < nitems(bad); i++) {
		cause = NULL;
		root = json_parse(bad[i], &cause);
		ASSERT(root == NULL);
		ASSERT_NOT_NULL(cause);
		free(cause);
		ASSERT(json_parse(bad[i], NULL) == NULL);
	}
}

TEST(depth_and_token_growth)
{
	char input[16384], *p = input, *cause = NULL;
	struct json_node *root;
	u_int i;

	/* Exceed the initial 1024-token allocation with a valid object array. */
	p += sprintf(p, "{\"panes\":[");
	for (i = 0; i < 400; i++)
		p += sprintf(p, "%s{\"id\":%u}", i ? "," : "", i);
	strcpy(p, "]}");
	root = json_parse(input, &cause);
	ASSERT_NOT_NULL(root);
	json_destroy_node(root);
	p = input;
	for (i = 0; i < 201; i++)
		p += sprintf(p, "{\"x\":");
	*p++ = '1';
	for (i = 0; i < 201; i++)
		*p++ = '}';
	*p = '\0';
	ASSERT(json_parse(input, &cause) == NULL);
	ASSERT_NOT_NULL(cause);
	free(cause);
}

TEST(typed_lookup_errors)
{
	struct json_node *root;
	char *cause = NULL;
	int64_t value = 42;

	root = json_parse("{\"x\":true}", &cause);
	ASSERT_NOT_NULL(root);
	ASSERT_EQ(json_find_number(root, "x", &value, &cause), -1);
	ASSERT_EQ(value, 42);
	ASSERT_NOT_NULL(cause);
	free(cause);
	cause = NULL;
	ASSERT_EQ(json_find_number(root, "missing", &value, &cause), -1);
	ASSERT_EQ(value, 42);
	ASSERT_NOT_NULL(cause);
	free(cause);
	ASSERT_EQ(json_find_number(root, "missing", &value, NULL), -1);
	json_destroy_node(root);
}

TEST(memory_copy_and_overflow_guard)
{
	const char bytes[] = {'x', '\0', (char)0xff};
	char *copy;
	pid_t child;
	int status;

	copy = xmemdup(bytes, sizeof bytes);
	ASSERT_EQ(memcmp(copy, bytes, sizeof bytes), 0);
	ASSERT_EQ(copy[sizeof bytes], '\0');
	free(copy);
	copy = xmemdup(NULL, 0);
	ASSERT_EQ(copy[0], '\0');
	free(copy);
	child = fork();
	ASSERT(child >= 0);
	if (child == 0) {
		(void)xcalloc(SIZE_MAX / 2 + 1, 2);
		_exit(1);
	}
	ASSERT_EQ(waitpid(child, &status, 0), child);
	ASSERT(WIFEXITED(status));
	ASSERT_EQ(WEXITSTATUS(status), 77);
}

int main(void)
{
	TEST_SUITE_BEGIN("Production JSON layout support");
	RUN_TEST(layout_shape_roundtrip);
	RUN_TEST(rejects_malformed_and_unsupported_inputs);
	RUN_TEST(depth_and_token_growth);
	RUN_TEST(typed_lookup_errors);
	RUN_TEST(memory_copy_and_overflow_guard);
	TEST_SUITE_END();
}
