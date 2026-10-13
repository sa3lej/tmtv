/* Complete host linkage: production parser, queue, options and ref cleanup.
 * Client fixtures own a real queue; no transport is needed for set-option.
 * Only tmux.c's program entrypoint is renamed, with no production stubs.
 */
#include "../tmux.h"
#include "test-harness.h"

static void
setup(void)
{
	const struct options_table_entry *oe;

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
}

static void
client_init(struct client *c)
{
	memset(c, 0, sizeof *c);
	c->name = "queue-test";
	c->references = 1; /* Fixture ownership; queued items use production refs. */
	c->queue = cmdq_new();
	c->environ = environ_create();
}

static struct cmdq_item *
command(const char *text)
{
	struct cmd_parse_result *pr = cmd_parse_from_string(text, NULL);
	struct cmdq_item *item;

	if (pr->status != CMD_PARSE_SUCCESS)
		fatalx("test command did not parse: %s", text);
	item = cmdq_get_command(pr->cmdlist, NULL);
	cmd_list_free(pr->cmdlist);
	return (item);
}

static enum cmd_retval
count_callback(struct cmdq_item *item, void *data)
{
	int *count = data;

	if (cmdq_get_cmd(item) != NULL)
		fatalx("callback has command");
	(*count)++;
	return (CMD_RETURN_NORMAL);
}

TEST(live_client_and_global_commands_execute)
{
	struct client c;
	struct cmdq_item *item;

	client_init(&c);
	item = command("set-option -g @queue-live yes");
	ASSERT_STR_EQ(cmd_get_entry(cmdq_get_cmd(item))->name, "set-option");
	cmdq_append(&c, item);
	ASSERT_EQ(c.references, 2);
	ASSERT_EQ(cmdq_next(&c), 1);
	ASSERT_STR_EQ(options_get_string(global_s_options, "@queue-live"), "yes");
	ASSERT_EQ(c.references, 1);
	ASSERT_EQ(cmdq_next(&c), 0);
	cmdq_free(c.queue);
	environ_free(c.environ);

	cmdq_append(NULL, command("set-option -g @queue-global yes"));
	ASSERT_EQ(cmdq_next(NULL), 1);
	ASSERT_STR_EQ(options_get_string(global_s_options, "@queue-global"), "yes");
}

TEST(dead_client_skips_grouped_commands_but_runs_cleanup)
{
	struct client c;
	int callbacks = 0;

	client_init(&c);
	cmdq_append(&c, command("set-option -g @queue-dead yes ; set-option -g @queue-dead-second yes"));
	cmdq_append(&c, cmdq_get_callback(count_callback, &callbacks));
	ASSERT_EQ(c.references, 4);
	c.flags |= CLIENT_DEAD; /* Disconnect before the queued command fires. */
	cmdq_next(&c);
	ASSERT_EQ(callbacks, 1);
	ASSERT_EQ(c.references, 1);
	ASSERT_EQ(cmdq_next(&c), 0);
	cmdq_free(c.queue);
	environ_free(c.environ);
	ASSERT(options_get_only(global_s_options, "@queue-dead") == NULL);
	ASSERT(options_get_only(global_s_options, "@queue-dead-second") == NULL);
}

static enum cmd_retval
disconnect_callback(struct cmdq_item *item, __unused void *data)
{
	cmdq_get_client(item)->flags |= CLIENT_DEAD;
	return (CMD_RETURN_NORMAL);
}

TEST(disconnect_during_drain_cancels_remaining_commands)
{
	struct client c;
	int callbacks = 0;

	client_init(&c);
	cmdq_append(&c, command("set-option -g @queue-before yes"));
	cmdq_append(&c, cmdq_get_callback(disconnect_callback, NULL));
	cmdq_append(&c, command("set-option -g @queue-after yes"));
	cmdq_append(&c, cmdq_get_callback(count_callback, &callbacks));
	ASSERT_EQ(cmdq_next(&c), 4);
	ASSERT_EQ(callbacks, 1);
	ASSERT_EQ(c.references, 1);
	ASSERT_EQ(cmdq_next(&c), 0);
	cmdq_free(c.queue);
	environ_free(c.environ);
	ASSERT_STR_EQ(options_get_string(global_s_options, "@queue-before"), "yes");
	ASSERT(options_get_only(global_s_options, "@queue-after") == NULL);
}

TEST(optional_argument_preserves_following_flags)
{
	struct cmd_parse_result *pr;
	struct args *args;
	const char *texts[] = {
		"refresh-client -l -S",
		"refresh-client -l -- target",
		"refresh-client -l -2",
		"refresh-client -lvalue -S"
	};
	u_int i;

	for (i = 0; i < nitems(texts); i++) {
		pr = cmd_parse_from_string(texts[i], NULL);
		ASSERT_EQ(pr->status, CMD_PARSE_SUCCESS);
		args = cmd_get_args(cmd_list_first(pr->cmdlist));
		ASSERT(args_has(args, 'l'));
		if (i == 0) {
			ASSERT(args_get(args, 'l') == NULL);
			ASSERT(args_has(args, 'S'));
		} else if (i == 1) {
			ASSERT(args_get(args, 'l') == NULL);
			ASSERT_EQ(args_count(args), 1);
			ASSERT_STR_EQ(args_string(args, 0), "target");
		} else if (i == 2)
			ASSERT_STR_EQ(args_get(args, 'l'), "-2");
		else {
			ASSERT_STR_EQ(args_get(args, 'l'), "value");
			ASSERT(args_has(args, 'S'));
		}
		cmd_list_free(pr->cmdlist);
	}
}

TEST(prepared_command_block_returns_owned_reference)
{
	struct cmdq_item *item;
	struct cmd *cmd;
	struct args_command_state *state;
	struct cmd_list *original, *result;
	char *error = NULL;
	int references;

	item = command("run-shell -C { set-option -g @prepared yes }");
	cmd = cmdq_get_cmd(item);
	original = args_value(cmd_get_args(cmd), 0)->cmdlist;
	references = original->references;
	state = args_make_commands_prepare(cmd, item, 0, NULL, 0, 0);
	ASSERT_EQ(original->references, references + 1);
	result = args_make_commands(state, 0, NULL, &error);
	ASSERT(result == original);
	ASSERT(error == NULL);
	ASSERT_EQ(original->references, references + 2);
	args_make_commands_free(state);
	ASSERT_EQ(original->references, references + 1);
	cmd_list_free(result);
	ASSERT_EQ(original->references, references);
	/* Drain through production queue so original command ownership is freed. */
	cmdq_append(NULL, item);
	while (cmdq_next(NULL) != 0)
		;
	event_loop(EVLOOP_NONBLOCK);
	while (cmdq_next(NULL) != 0)
		;
	ASSERT_STR_EQ(options_get_string(global_s_options, "@prepared"), "yes");
}

TEST(immediate_string_command_has_single_owner)
{
	struct cmdq_item *item;
	struct cmd_list *result;

	item = command("if-shell -F 1 'set-option -g @immediate yes'");
	result = args_make_commands_now(cmdq_get_cmd(item), item, 1, 0);
	ASSERT_NOT_NULL(result);
	ASSERT_EQ(result->references, 1);
	cmd_list_free(result);
	cmdq_append(NULL, item);
	while (cmdq_next(NULL) != 0)
		;
	ASSERT_STR_EQ(options_get_string(global_s_options, "@immediate"), "yes");
	cmdq_append(NULL, command("if-shell -F 1 { set-option -g @immediate-block yes }"));
	while (cmdq_next(NULL) != 0)
		;
	ASSERT_STR_EQ(options_get_string(global_s_options, "@immediate-block"), "yes");
}

TEST(percentage_growth_respects_output_bounds)
{
	char *cause = NULL;
	struct cmdq_item *item;

	ASSERT_EQ(args_string_percentage("250%", 0, 1000, 80, &cause), 200);
	ASSERT(cause == NULL);
	ASSERT_EQ(args_string_percentage("1000%", 0, 1000, 80, &cause), 800);
	ASSERT(cause == NULL);
	ASSERT_EQ(args_string_percentage("1001%", 0, 1000, 80, &cause), 0);
	ASSERT_NOT_NULL(cause);
	free(cause);
	ASSERT_EQ(args_string_percentage("250%", 0, 100, 80, &cause), 0);
	ASSERT_STR_EQ(cause, "too large");
	free(cause);
	item = command("set-option -g @percentage yes");
	ASSERT_EQ(args_string_percentage_and_expand("#{e|*:125,2}%", 0,
	    1000, 80, item, &cause), 200);
	ASSERT(cause == NULL);
	cmdq_append(NULL, item);
	while (cmdq_next(NULL) != 0)
		;
}

TEST(packed_argv_rejects_invalid_counts_and_empty_payload)
{
	char **argv = NULL;
	char buf[1001] = {0};

	ASSERT_EQ(cmd_unpack_argv(NULL, 0, 0, &argv), 0);
	ASSERT(argv == NULL);
	ASSERT_EQ(cmd_unpack_argv(buf, sizeof buf, 1001, &argv), -1);
	ASSERT(argv == NULL);
	ASSERT_EQ(cmd_unpack_argv(buf, sizeof buf, -1, &argv), -1);
	ASSERT(argv == NULL);
	ASSERT_EQ(cmd_unpack_argv(NULL, 0, 1, &argv), -1);
	ASSERT(argv == NULL);
	ASSERT_EQ(cmd_unpack_argv(buf, 1000, 1000, &argv), 0);
	ASSERT_STR_EQ(argv[999], "");
	cmd_free_argv(1000, argv);
}

TEST(packed_argv_roundtrip_and_truncation)
{
	char *input[] = { "set-option", "-g", "@packed", "a 'quote'; value", "" };
	char **argv = NULL;
	char buf[64];
	size_t len = 0;
	u_int i;

	for (i = 0; i < nitems(input); i++)
		len += strlen(input[i]) + 1;
	ASSERT_EQ(cmd_pack_argv(nitems(input), input, buf, len), 0);
	ASSERT_EQ(cmd_unpack_argv(buf, len, nitems(input), &argv), 0);
	for (i = 0; i < nitems(input); i++)
		ASSERT_STR_EQ(argv[i], input[i]);
	cmd_free_argv(nitems(input), argv);
	ASSERT_EQ(cmd_unpack_argv(buf, len, nitems(input) + 1, &argv), -1);
	/* Rejection already frees partially populated argv. */
	ASSERT_EQ(cmd_pack_argv(nitems(input), input, buf, len - 1), -1);
}

TEST(template_quote_modes_and_placeholder_selection)
{
	char *value;
	const char *templates[] = { "'%%'", "\"%%%\"", "\"%1%\"", "%1", "%% %% %1 %2 %", "literal" };
	const char *expected[] = { "'a'\\''b'", "\"a'b\"", "\"a'b\"", "a'b", "a'\\''b %% a'b %2 %", "literal" };
	u_int i;

	for (i = 0; i < nitems(templates); i++) {
		value = cmd_template_replace(templates[i], "a'b", 1);
		ASSERT_STR_EQ(value, expected[i]);
		free(value);
	}
	value = cmd_template_replace("%%% %2% %2 %1", "\"\\$;~", 2);
	ASSERT_STR_EQ(value, "\\\"\\\\\\$\\;\\~ \\\"\\\\\\$\\;\\~ \"\\$;~ %1");
	free(value);
}

TEST(prepared_template_executes_literal_without_injection)
{
	const char *templates[] = {
		"set-option -g @template-value '%%'",
		"set-option -g @template-value \"%%%\"",
		"set-option -g @template-value \"%1%\""
	};
	char *payload[] = { "a' ; set-option -g @template-injected yes ; # \"\\$;~\nend" };
	struct cmdq_item *item;
	struct args_command_state *state;
	struct cmd_list *result;
	char *error = NULL;
	u_int i;

	for (i = 0; i < nitems(templates); i++) {
		item = command("set-option -g @template-parent yes");
		state = args_make_commands_prepare(cmdq_get_cmd(item), item,
		    2, templates[i], 0, 0);
		result = args_make_commands(state, 1, payload, &error);
		ASSERT(result != NULL && error == NULL);
		cmdq_append(NULL, cmdq_get_command(result, NULL));
		cmd_list_free(result);
		args_make_commands_free(state);
		cmdq_append(NULL, item);
		while (cmdq_next(NULL) != 0)
			;
		ASSERT_STR_EQ(options_get_string(global_s_options,
		    "@template-value"), payload[0]);
		ASSERT(options_get_only(global_s_options, "@template-injected") == NULL);
	}
}

int
main(void)
{
	setup();
	TEST_SUITE_BEGIN("Production command queue and arguments");
	RUN_TEST(packed_argv_rejects_invalid_counts_and_empty_payload);
	RUN_TEST(packed_argv_roundtrip_and_truncation);
	RUN_TEST(template_quote_modes_and_placeholder_selection);
	RUN_TEST(prepared_template_executes_literal_without_injection);
	RUN_TEST(optional_argument_preserves_following_flags);
	RUN_TEST(prepared_command_block_returns_owned_reference);
	RUN_TEST(immediate_string_command_has_single_owner);
	RUN_TEST(percentage_growth_respects_output_bounds);
	RUN_TEST(live_client_and_global_commands_execute);
	RUN_TEST(dead_client_skips_grouped_commands_but_runs_cleanup);
	RUN_TEST(disconnect_during_drain_cancels_remaining_commands);
	while (cmdq_next(NULL) != 0)
		;
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	TEST_SUITE_END();
}
