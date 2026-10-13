/* Complete production host linkage; only tmux.c's entrypoint is renamed. */
#include "../tmux.h"
#include "test-harness.h"

static struct event_base *base;

static void
setup(void)
{
	const struct options_table_entry *oe;

	base = event_init();
	socket_path = xstrdup("/isolated-options-hook-test");
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
drain(void)
{
	while (cmdq_next(NULL) != 0)
		;
}

static enum cmd_retval
fire_notification(struct cmdq_item *item, void *data)
{
	hooks_run(item, data);
	return (CMD_RETURN_NORMAL);
}

TEST(notification_hooks_count_once_and_execute)
{
	struct options_entry *o;

	o = options_set_string(global_s_options, "@options-hook", 0,
	    "set-option -g @notification-result yes");
	ASSERT_EQ(options_get_fire_count(o), 0);
	ASSERT_EQ(options_get_fire_time(o), 0);
	current_time = 1234;
	cmdq_append(NULL, cmdq_get_callback(fire_notification, "@options-hook"));
	drain();
	ASSERT_EQ(options_get_fire_count(o), 1);
	ASSERT_EQ(options_get_fire_time(o), 1234);
	ASSERT_STR_EQ(options_get_string(global_s_options,
	    "@notification-result"), "yes");
	current_time = 5678;
	cmdq_append(NULL, cmdq_get_callback(fire_notification, "@options-hook"));
	drain();
	ASSERT_EQ(options_get_fire_count(o), 2);
	ASSERT_EQ(options_get_fire_time(o), 5678);
	/* Updating the same entry preserves its metadata; removal resets it. */
	options_set_string(global_s_options, "@options-hook", 0, "%s", "");
	ASSERT_EQ(options_get_fire_count(o), 2);
	ASSERT_EQ(options_remove_or_default(o, NULL, NULL), 0);
	o = options_set_string(global_s_options, "@options-hook", 0, "%s", "");
	ASSERT_EQ(options_get_fire_count(o), 0);
	ASSERT_EQ(options_get_fire_time(o), 0);
}

static struct cmdq_item *
command(int flags)
{
	struct cmd_parse_result *pr;
	struct cmdq_state *state = cmdq_new_state(NULL, NULL, flags);
	struct cmdq_item *item;

	pr = cmd_parse_from_string("set-option -g @command-result yes", NULL);
	if (pr->status != CMD_PARSE_SUCCESS)
		fatalx("test command failed to parse");
	item = cmdq_get_command(pr->cmdlist, state);
	cmd_list_free(pr->cmdlist);
	cmdq_free_state(state);
	return (item);
}

TEST(command_hooks_multi_entry_and_nohooks)
{
	struct options_entry *o = options_get(global_s_options, "command-error");
	struct cmdq_item *item;

	options_array_clear(o);
	ASSERT_EQ(options_array_set(o, "0",
	    "set-option -g @command-hook-first yes", 0, NULL), 0);
	ASSERT_EQ(options_array_set(o, "3",
	    "set-option -g @command-hook-second yes", 0, NULL), 0);
	current_time = 9012;
	item = cmdq_append(NULL, command(0));
	cmdq_insert_hook(NULL, item, NULL, "%s", "command-error");
	drain();
	ASSERT_EQ(options_get_fire_count(o), 1);
	ASSERT_EQ(options_get_fire_time(o), 9012);
	ASSERT_STR_EQ(options_get_string(global_s_options,
	    "@command-hook-first"), "yes");
	ASSERT_STR_EQ(options_get_string(global_s_options,
	    "@command-hook-second"), "yes");
	current_time = 9999;
	item = cmdq_append(NULL, command(CMDQ_STATE_NOHOOKS));
	cmdq_insert_hook(NULL, item, NULL, "%s", "command-error");
	drain();
	ASSERT_EQ(options_get_fire_count(o), 1);
	ASSERT_EQ(options_get_fire_time(o), 9012);
	options_array_clear(o);
}

TEST(deferred_notification_accounting)
{
	struct options_entry *o;
	struct window *w = window_create(80, 24, 0, 0);
	struct window_pane *wp = window_add_pane(w, NULL, 100, 0);

	window_add_ref(w, __func__);
	window_set_active_pane(w, wp, 0);
	drain();
	o = options_set_string(global_s_options, "@deferred-hook", 0,
	    "set-option -g @deferred-result yes");
	current_time = 2468;
	events_fire_window("@deferred-hook", w);
	ASSERT_EQ(options_get_fire_count(o), 0);
	drain();
	ASSERT_EQ(options_get_fire_count(o), 1);
	ASSERT_EQ(options_get_fire_time(o), 2468);
	ASSERT_STR_EQ(options_get_string(global_s_options, "@deferred-result"), "yes");
	window_remove_ref(w, __func__);
	drain();
}

TEST(inheritance_shadowing_and_exact_search)
{
	struct options *child = options_create(global_s_options);
	struct options_entry *parent, *local;

	ASSERT(options_search("command-error") ==
	    options_table_entry(options_get(global_s_options, "command-error")));
	ASSERT(options_search("command-err") == NULL);
	ASSERT(options_search("@options-hook") == NULL);
	parent = options_get(child, "@options-hook");
	current_time = 4321;
	options_hook_fired(parent);
	ASSERT_EQ(options_get_fire_count(options_get(child, "@options-hook")), 1);
	local = options_set_string(child, "@options-hook", 0, "%s", "");
	ASSERT_EQ(options_get_fire_count(local), 0);
	current_time = 8765;
	options_hook_fired(local);
	ASSERT_EQ(options_get_fire_time(local), 8765);
	ASSERT_EQ(options_get_fire_time(parent), 4321);
	ASSERT_EQ(options_remove_or_default(local, NULL, NULL), 0);
	ASSERT(options_get(child, "@options-hook") == parent);
	options_free(child);
}

int
main(void)
{
	setup();
	TEST_SUITE_BEGIN("Production option hook firing metadata");
	RUN_TEST(notification_hooks_count_once_and_execute);
	RUN_TEST(command_hooks_multi_entry_and_nohooks);
	RUN_TEST(deferred_notification_accounting);
	RUN_TEST(inheritance_shadowing_and_exact_search);
	drain();
	options_free(global_w_options);
	options_free(global_s_options);
	options_free(global_options);
	environ_free(global_environ);
	free((void *)socket_path);
	event_base_free(base);
	TEST_SUITE_END();
}
