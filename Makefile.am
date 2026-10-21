# Obvious program stuff.
bin_PROGRAMS = tmtv
if IS_LINUX
bin_PROGRAMS += tmtv-server
endif
CLEANFILES = tmtv.1.mdoc tmtv.1.man cmd-parse.c

# Distribution tarball options.
EXTRA_DIST = \
	CHANGES README README.ja LICENSE example_tmux.conf \
	osdep-*.c mdoc2man.awk tmtv.1
dist_EXTRA_tmtv_SOURCES = compat/*.[ch]

# Preprocessor flags.
AM_CPPFLAGS += @XOPEN_DEFINES@ \
	-DTMUX_VERSION='"3.8"' \
	-DTMTV_VERSION='"@VERSION@"' \
	-DTMUX_CONF='"$(sysconfdir)/tmtv.conf:~/.tmtv.conf:$$XDG_CONFIG_HOME/tmtv/tmtv.conf:~/.config/tmtv/tmtv.conf"' \
	-DTMUX_LOCK_CMD='"@DEFAULT_LOCK_CMD@"' \
	-DTMUX_TERM='"@DEFAULT_TERM@"' \
	-DTMUX_MOUSE=1

# Additional object files.
LDADD = $(LIBOBJS)

# Set flags for gcc.
if IS_GCC
AM_CFLAGS += -std=gnu99
if IS_OPTIMIZED
AM_CFLAGS += -O2
else
AM_CFLAGS += -O0
endif
if IS_DEBUG
AM_CFLAGS += -g3 -ggdb
AM_CFLAGS += -Wno-long-long -Wall -W -Wformat=2 -Wno-use-after-free
AM_CFLAGS += -Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations
AM_CFLAGS += -Wwrite-strings -Wshadow -Wpointer-arith -Wsign-compare
AM_CFLAGS += -Wundef -Wbad-function-cast -Winline -Wno-cast-align
AM_CFLAGS += -Wdeclaration-after-statement -Wno-pointer-sign -Wno-attributes
AM_CFLAGS += -Wno-unused-result -Wno-format-y2k -Wno-unknown-warning-option
AM_CFLAGS += -Wno-maybe-uninitialized
if IS_DARWIN
AM_CFLAGS += -Wno-deprecated-declarations -Wno-macro-redefined
endif
AM_CPPFLAGS += -DDEBUG
endif
AM_CPPFLAGS += -iquote.
if IS_ASAN
AM_CPPFLAGS += -DASAN
AM_CFLAGS += -fsanitize=address -fno-omit-frame-pointer
AM_LDFLAGS += -fsanitize=address
endif
endif

# Set flags for Solaris.
if IS_SUNOS
if IS_GCC
AM_CPPFLAGS += -D_XPG6
else
AM_CPPFLAGS += -D_XPG4_2
endif
endif

# Set flags for Sun CC.
if IS_SUNCC
AM_CFLAGS += -erroff=E_EMPTY_DECLARATION
endif

# Set _LINUX_SOURCE_COMPAT for AIX for malloc(0).
if IS_AIX
AM_CPPFLAGS += -D_LINUX_SOURCE_COMPAT=1
endif

# Set flags for NetBSD.
if IS_NETBSD
AM_CPPFLAGS += -D_OPENBSD_SOURCE
endif

# Set flags for Haiku.
if IS_HAIKU
AM_CPPFLAGS += -D_BSD_SOURCE
endif

# Set flags for Cygwin.
if IS_CYGWIN
AM_CPPFLAGS += -DTMUX_SOCK_PERM=0
endif

# List of sources.
dist_tmtv_SOURCES = \
	alerts.c \
	arguments.c \
	attributes.c \
	cfg.c \
	client.c \
	cmd-attach-session.c \
	cmd-bind-key.c \
	cmd-break-pane.c \
	cmd-capture-pane.c \
	cmd-choose-tree.c \
	cmd-command-prompt.c \
	cmd-confirm-before.c \
	cmd-copy-mode.c \
	cmd-detach-client.c \
	cmd-display-menu.c \
	cmd-display-message.c \
	cmd-find-window.c \
	cmd-find.c \
	cmd-if-shell.c \
	cmd-join-pane.c \
	cmd-kill-pane.c \
	cmd-kill-server.c \
	cmd-kill-session.c \
	cmd-kill-window.c \
	cmd-list-buffers.c \
	cmd-list-clients.c \
	cmd-list-commands.c \
	cmd-list-keys.c \
	cmd-list-panes.c \
	cmd-list-sessions.c \
	cmd-list-windows.c \
	cmd-load-buffer.c \
	cmd-lock-server.c \
	cmd-move-window.c \
	cmd-new-session.c \
	cmd-new-window.c \
	cmd-parse.y \
	cmd-paste-buffer.c \
	cmd-pipe-pane.c \
	cmd-queue.c \
	cmd-refresh-client.c \
	cmd-rename-session.c \
	cmd-rename-window.c \
	cmd-resize-pane.c \
	cmd-resize-window.c \
	cmd-respawn-pane.c \
	cmd-respawn-window.c \
	cmd-rotate-window.c \
	cmd-run-shell.c \
	cmd-save-buffer.c \
	cmd-select-layout.c \
	cmd-select-pane.c \
	cmd-select-window.c \
	cmd-send-keys.c \
	cmd-server-access.c \
	cmd-set-buffer.c \
	cmd-set-environment.c \
	cmd-set-option.c \
	cmd-show-environment.c \
	cmd-show-messages.c \
	cmd-show-options.c \
	cmd-show-prompt-history.c \
	cmd-source-file.c \
	cmd-split-window.c \
	cmd-swap-pane.c \
	cmd-swap-window.c \
	cmd-switch-client.c \
	cmd-unbind-key.c \
	cmd-wait-for.c \
	cmd.c \
	colour.c \
	compat.h \
	control-notify.c \
	control.c \
	environ.c \
	events-payload.c \
	events.c \
	file.c \
	format-draw.c \
	format.c \
	fuzzy.c \
	grid-reader.c \
	grid-view.c \
	grid.c \
	hooks.c \
	hyperlinks.c \
	input-keys.c \
	input.c \
	job.c \
	json.c \
	key-bindings.c \
	key-string.c \
	layout-custom.c \
	layout-set.c \
	layout.c \
	log.c \
	menu.c \
	mode-tree.c \
	monitor.c \
	tmate-layout.c \
	tmate-layout.h \
	names.c \
	options-table.c \
	options.c \
	paste.c \
	popup.c \
	proc.c \
	prompt-history.c \
	prompt.c \
	regsub.c \
	resize.c \
	screen-redraw.c \
	screen-write.c \
	screen.c \
	server-acl.c \
	server-client.c \
	server-fn.c \
	server.c \
	session.c \
	sort.c \
	spawn.c \
	status.c \
	style.c \
	tmux-protocol.h \
	tmux.c \
	tmux.h \
	tty-acs.c \
	tty-draw.c \
	tty-features.c \
	tty-keys.c \
	tty-term.c \
	tty.c \
	utf8-combined.c \
	utf8.c \
	window-border.c \
	window-buffer.c \
	window-client.c \
	window-clock.c \
	window-copy.c \
	window-customize.c \
	window-panes.c \
	window-switch.c \
	window-tree.c \
	window-visible.c \
	window.c \
	xmalloc.c \
	xmalloc.h \
	tmate.h \
	tmate-protocol.h \
	tmate-key.h \
	tmate-debug.c \
	tmate-decoder.c \
	tmate-encoder.c \
	tmate-env.c \
	tmate-msg.c \
	tmate-msgpack.c \
	tmate-recording.c \
	tmate-input-socket.c \
	tmate-session.c \
	tmate-ssh-client.c
nodist_tmtv_SOURCES = osdep-@PLATFORM@.c

# tmtv-server: tmate SSH server binary (Linux only).
# Shares tmux core sources with tmtv, but uses server/ tmate files
# instead of client-side tmate files, and has its own main().
if IS_LINUX
tmtv_server_CPPFLAGS = -DTMATE_SERVER_BUILD $(AM_CPPFLAGS)
dist_tmtv_server_SOURCES = \
	alerts.c \
	arguments.c \
	attributes.c \
	cfg.c \
	client.c \
	cmd-attach-session.c \
	cmd-bind-key.c \
	cmd-break-pane.c \
	cmd-capture-pane.c \
	cmd-choose-tree.c \
	cmd-command-prompt.c \
	cmd-confirm-before.c \
	cmd-copy-mode.c \
	cmd-detach-client.c \
	cmd-display-menu.c \
	cmd-display-message.c \
	cmd-find-window.c \
	cmd-find.c \
	cmd-if-shell.c \
	cmd-join-pane.c \
	cmd-kill-pane.c \
	cmd-kill-server.c \
	cmd-kill-session.c \
	cmd-kill-window.c \
	cmd-list-buffers.c \
	cmd-list-clients.c \
	cmd-list-commands.c \
	cmd-list-keys.c \
	cmd-list-panes.c \
	cmd-list-sessions.c \
	cmd-list-windows.c \
	cmd-load-buffer.c \
	cmd-lock-server.c \
	cmd-move-window.c \
	cmd-new-session.c \
	cmd-new-window.c \
	cmd-parse.y \
	cmd-paste-buffer.c \
	cmd-pipe-pane.c \
	cmd-queue.c \
	cmd-refresh-client.c \
	cmd-rename-session.c \
	cmd-rename-window.c \
	cmd-resize-pane.c \
	cmd-resize-window.c \
	cmd-respawn-pane.c \
	cmd-respawn-window.c \
	cmd-rotate-window.c \
	cmd-run-shell.c \
	cmd-save-buffer.c \
	cmd-select-layout.c \
	cmd-select-pane.c \
	cmd-select-window.c \
	cmd-send-keys.c \
	cmd-server-access.c \
	cmd-set-buffer.c \
	cmd-set-environment.c \
	cmd-set-option.c \
	cmd-show-environment.c \
	cmd-show-messages.c \
	cmd-show-options.c \
	cmd-show-prompt-history.c \
	cmd-source-file.c \
	cmd-split-window.c \
	cmd-swap-pane.c \
	cmd-swap-window.c \
	cmd-switch-client.c \
	cmd-unbind-key.c \
	cmd-wait-for.c \
	cmd.c \
	colour.c \
	compat.h \
	control-notify.c \
	control.c \
	environ.c \
	events-payload.c \
	events.c \
	file.c \
	format-draw.c \
	format.c \
	fuzzy.c \
	grid-reader.c \
	grid-view.c \
	grid.c \
	hooks.c \
	hyperlinks.c \
	input-keys.c \
	input.c \
	job.c \
	json.c \
	key-bindings.c \
	key-string.c \
	layout-custom.c \
	layout-set.c \
	layout.c \
	log.c \
	menu.c \
	mode-tree.c \
	monitor.c \
	tmate-layout.c \
	tmate-layout.h \
	names.c \
	options-table.c \
	options.c \
	paste.c \
	popup.c \
	proc.c \
	prompt-history.c \
	prompt.c \
	regsub.c \
	resize.c \
	screen-redraw.c \
	screen-write.c \
	screen.c \
	server-acl.c \
	server-client.c \
	server-fn.c \
	server.c \
	session.c \
	sort.c \
	spawn.c \
	status.c \
	style.c \
	tmux-protocol.h \
	tmux.c \
	tmux.h \
	tty-acs.c \
	tty-draw.c \
	tty-features.c \
	tty-keys.c \
	tty-term.c \
	tty.c \
	utf8-combined.c \
	utf8.c \
	window-border.c \
	window-buffer.c \
	window-client.c \
	window-clock.c \
	window-copy.c \
	window-customize.c \
	window-panes.c \
	window-switch.c \
	window-tree.c \
	window-visible.c \
	window.c \
	xmalloc.c \
	xmalloc.h \
	tmate.h \
	tmate-protocol.h \
	tmate-key.h \
	server/tmate.h \
	server/tmate-auth-keys.c \
	server/tmate-daemon-decoder.c \
	server/tmate-daemon-encoder.c \
	server/tmate-daemon-legacy.c \
	server/tmate-debug.c \
	server/tmate-main.c \
	server/tmate-msgpack.c \
	server/tmate-rand.c \
	server/tmate-resume.c \
	server/tmate-resume.h \
	server/tmate-ssh-client-pty.c \
	server/tmate-ssh-daemon.c \
	server/tmate-ssh-exec.c \
	server/tmate-ssh-latency.c \
	server/tmate-ssh-server.c \
	server/tmate-sse-mux.c \
	server/tmate-websocket.c \
	server/tmate-client-stubs.c
nodist_tmtv_server_SOURCES = osdep-@PLATFORM@.c
endif

# Add compat file for forkpty.
if NEED_FORKPTY
nodist_tmtv_SOURCES += compat/forkpty-@PLATFORM@.c
if IS_LINUX
nodist_tmtv_server_SOURCES += compat/forkpty-@PLATFORM@.c
endif
endif

# Add compat file for systemd.
if HAVE_SYSTEMD
nodist_tmtv_SOURCES += compat/systemd.c
if IS_LINUX
nodist_tmtv_server_SOURCES += compat/systemd.c
endif
endif

# Add compat file for utf8proc.
if HAVE_UTF8PROC
nodist_tmtv_SOURCES += compat/utf8proc.c
if IS_LINUX
nodist_tmtv_server_SOURCES += compat/utf8proc.c
endif
endif

# Enable sixel support.
if ENABLE_SIXEL
dist_tmtv_SOURCES += image.c image-sixel.c
if IS_LINUX
dist_tmtv_server_SOURCES += image.c image-sixel.c
endif
endif

# Add bits for fuzzing if enabled.
if NEED_FUZZING
check_PROGRAMS = \
	fuzz/input-fuzzer \
	fuzz/cmd-parse-fuzzer \
	fuzz/format-fuzzer \
	fuzz/style-fuzzer
fuzz_input_fuzzer_LDFLAGS = $(FUZZING_LIBS)
fuzz_input_fuzzer_LDADD = $(LDADD) $(tmtv_OBJECTS)
fuzz_cmd_parse_fuzzer_LDFLAGS = $(FUZZING_LIBS)
fuzz_cmd_parse_fuzzer_LDADD = $(LDADD) $(tmtv_OBJECTS)
fuzz_format_fuzzer_LDFLAGS = $(FUZZING_LIBS)
fuzz_format_fuzzer_LDADD = $(LDADD) $(tmtv_OBJECTS)
fuzz_style_fuzzer_LDFLAGS = $(FUZZING_LIBS)
fuzz_style_fuzzer_LDADD = $(LDADD) $(tmtv_OBJECTS)
endif

# Install tmtv.1 in the right format.
install-exec-hook:
	if test x@MANFORMAT@ = xmdoc; then \
		sed -e "s|@SYSCONFDIR@|$(sysconfdir)|g" $(srcdir)/tmtv.1 \
			>$(srcdir)/tmtv.1.mdoc; \
	else \
		sed -e "s|@SYSCONFDIR@|$(sysconfdir)|g" $(srcdir)/tmtv.1| \
			$(AWK) -f $(srcdir)/mdoc2man.awk >$(srcdir)/tmtv.1.man; \
	fi
	$(mkdir_p) $(DESTDIR)$(mandir)/man1
	$(INSTALL_DATA) $(srcdir)/tmtv.1.@MANFORMAT@ \
		$(DESTDIR)$(mandir)/man1/tmtv.1

# Exercise pane lifetimes with all host production objects, without its entrypoint.
tests/test-window-tmux.o: tmux.c tmux.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -Dmain=tmtv_program_main -c $(srcdir)/tmux.c -o $@

tests/test-window-lifetime.o: tests/test-window-lifetime.c tmux.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $(srcdir)/tests/test-window-lifetime.c -o $@

tests/test-window-lifetime: tests/test-window-lifetime.o tests/test-window-tmux.o $(tmtv_OBJECTS) $(tmtv_DEPENDENCIES)
	$(AM_V_CCLD)$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ tests/test-window-lifetime.o tests/test-window-tmux.o $(filter-out tmux.o,$(tmtv_OBJECTS)) $(tmtv_LDADD) $(LIBS)

CLEANFILES += tests/test-window-lifetime tests/test-window-lifetime.o tests/test-window-tmux.o
EXTRA_DIST += tests/test-window-lifetime.c

# Exercise event payloads through the complete production host.
tests/test-event-payload.o: tests/test-event-payload.c tmux.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $(srcdir)/tests/test-event-payload.c -o $@

tests/test-event-payload: tests/test-event-payload.o tests/test-window-tmux.o $(tmtv_OBJECTS) $(tmtv_DEPENDENCIES)
	$(AM_V_CCLD)$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ tests/test-event-payload.o tests/test-window-tmux.o $(filter-out tmux.o,$(tmtv_OBJECTS)) $(tmtv_LDADD) $(LIBS)

CLEANFILES += tests/test-event-payload tests/test-event-payload.o
EXTRA_DIST += tests/test-event-payload.c
EXTRA_DIST += tests/run-relay-e2e.py tests/test-relay-e2e.py \
	tests/requirements-relay-e2e.txt

# Exercise event dispatch through the complete production host.
tests/test-events.o: tests/test-events.c tmux.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $(srcdir)/tests/test-events.c -o $@

tests/test-events: tests/test-events.o tests/test-window-tmux.o $(tmtv_OBJECTS) $(tmtv_DEPENDENCIES)
	$(AM_V_CCLD)$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ tests/test-events.o tests/test-window-tmux.o $(filter-out tmux.o,$(tmtv_OBJECTS)) $(tmtv_LDADD) $(LIBS)

CLEANFILES += tests/test-events tests/test-events.o
EXTRA_DIST += tests/test-events.c

# Exercise parsed commands and disconnect cancellation in the production queue.
tests/test-cmd-queue.o: tests/test-cmd-queue.c tmux.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $(srcdir)/tests/test-cmd-queue.c -o $@

tests/test-cmd-queue: tests/test-cmd-queue.o tests/test-window-tmux.o $(tmtv_OBJECTS) $(tmtv_DEPENDENCIES)
	$(AM_V_CCLD)$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ tests/test-cmd-queue.o tests/test-window-tmux.o $(filter-out tmux.o,$(tmtv_OBJECTS)) $(tmtv_LDADD) $(LIBS)

CLEANFILES += tests/test-cmd-queue tests/test-cmd-queue.o
EXTRA_DIST += tests/test-cmd-queue.c

# Exercise hook accounting through production options, notification and queue paths.
tests/test-options-hooks.o: tests/test-options-hooks.c tmux.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $(srcdir)/tests/test-options-hooks.c -o $@

tests/test-options-hooks: tests/test-options-hooks.o tests/test-window-tmux.o $(tmtv_OBJECTS) $(tmtv_DEPENDENCIES)
	$(AM_V_CCLD)$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ tests/test-options-hooks.o tests/test-window-tmux.o $(filter-out tmux.o,$(tmtv_OBJECTS)) $(tmtv_LDADD) $(LIBS)

CLEANFILES += tests/test-options-hooks tests/test-options-hooks.o
EXTRA_DIST += tests/test-options-hooks.c

# Exercise option-owned monitors with complete host production objects.
tests/test-monitor.o: tests/test-monitor.c tmux.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $(srcdir)/tests/test-monitor.c -o $@

tests/test-monitor: tests/test-monitor.o tests/test-window-tmux.o $(tmtv_OBJECTS) $(tmtv_DEPENDENCIES)
	$(AM_V_CCLD)$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ tests/test-monitor.o tests/test-window-tmux.o $(filter-out tmux.o,$(tmtv_OBJECTS)) $(tmtv_LDADD) $(LIBS)

CLEANFILES += tests/test-monitor tests/test-monitor.o
EXTRA_DIST += tests/test-monitor.c

tests/test-control-subscriptions.o: tests/test-control-subscriptions.c tmux.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $(srcdir)/tests/test-control-subscriptions.c -o $@

tests/test-control-subscriptions: tests/test-control-subscriptions.o tests/test-window-tmux.o $(tmtv_OBJECTS) $(tmtv_DEPENDENCIES)
	$(AM_V_CCLD)$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ tests/test-control-subscriptions.o tests/test-window-tmux.o $(filter-out tmux.o,$(tmtv_OBJECTS)) $(tmtv_LDADD) $(LIBS)

CLEANFILES += tests/test-control-subscriptions tests/test-control-subscriptions.o
EXTRA_DIST += tests/test-control-subscriptions.c

tests/test-mode-lifecycle.o: tests/test-mode-lifecycle.c tmux.h tmate-key.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(AM_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $(srcdir)/tests/test-mode-lifecycle.c -o $@

tests/test-mode-lifecycle: tests/test-mode-lifecycle.o tests/test-window-tmux.o $(tmtv_OBJECTS) $(tmtv_DEPENDENCIES)
	$(AM_V_CCLD)$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ tests/test-mode-lifecycle.o tests/test-window-tmux.o $(filter-out tmux.o,$(tmtv_OBJECTS)) $(tmtv_LDADD) $(LIBS)

CLEANFILES += tests/test-mode-lifecycle tests/test-mode-lifecycle.o
EXTRA_DIST += tests/test-mode-lifecycle.c

# Link the complete relay, including its actual MessagePack dispatcher.
tests/test-relay-tmux.o: tmux.c tmux.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(tmtv_server_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -Dmain=tmtv_program_main -c $(srcdir)/tmux.c -o $@

tests/test-relay-copy-output.o: tests/test-relay-copy-output.c tests/test-layout.h tmux.h server/tmate.h tmate-layout.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(tmtv_server_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -c $(srcdir)/tests/test-relay-copy-output.c -o $@

tests/test-relay-main.o: server/tmate-main.c tmux.h server/tmate.h
	$(AM_V_CC)$(CC) $(DEFS) $(DEFAULT_INCLUDES) $(INCLUDES) $(tmtv_server_CPPFLAGS) $(CPPFLAGS) $(AM_CFLAGS) $(CFLAGS) -Dmain=tmtv_relay_main -c $(srcdir)/server/tmate-main.c -o $@

tests/test-relay-copy-output: tests/test-relay-copy-output.o tests/test-relay-tmux.o tests/test-relay-main.o $(tmtv_server_OBJECTS) $(tmtv_server_DEPENDENCIES)
	$(AM_V_CCLD)$(CC) $(AM_CFLAGS) $(CFLAGS) $(AM_LDFLAGS) $(LDFLAGS) -o $@ tests/test-relay-copy-output.o tests/test-relay-tmux.o tests/test-relay-main.o $(filter-out tmtv_server-tmux.o server/tmtv_server-tmate-main.o,$(tmtv_server_OBJECTS)) $(tmtv_server_LDADD) $(LIBS)

CLEANFILES += tests/test-relay-copy-output tests/test-relay-copy-output.o tests/test-relay-tmux.o tests/test-relay-main.o
EXTRA_DIST += tests/test-relay-copy-output.c tests/test-layout.h

EXTRA_DIST += tests/test-core38-commands.py tests/run-tmux38-regress.sh
EXTRA_DIST += docs/layout-protocol.md
