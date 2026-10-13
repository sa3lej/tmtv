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

int
main(void)
{
	setup();
	TEST_SUITE_BEGIN("Production command queue disconnect handling");
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
