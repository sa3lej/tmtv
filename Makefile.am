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
	-DTMUX_VERSION='"3.6a"' \
	-DTMTV_VERSION='"@VERSION@"' \
	-DTMUX_CONF='"$(sysconfdir)/tmtv.conf:~/.tmtv.conf:$$XDG_CONFIG_HOME/tmtv/tmtv.conf:~/.config/tmtv/tmtv.conf"' \
	-DTMUX_LOCK_CMD='"@DEFAULT_LOCK_CMD@"' \
	-DTMUX_TERM='"@DEFAULT_TERM@"'

# Additional object files.
LDADD = $(LIBOBJS)

# Set flags for gcc.
if IS_GCC
AM_CFLAGS += -std=gnu99 -O2
if IS_DEBUG
AM_CFLAGS += -g
AM_CFLAGS += -Wno-long-long -Wall -W -Wformat=2
AM_CFLAGS += -Wmissing-prototypes -Wstrict-prototypes -Wmissing-declarations
AM_CFLAGS += -Wwrite-strings -Wshadow -Wpointer-arith -Wsign-compare
AM_CFLAGS += -Wundef -Wbad-function-cast -Winline -Wcast-align
AM_CFLAGS += -Wdeclaration-after-statement -Wno-pointer-sign -Wno-attributes
AM_CFLAGS += -Wno-unused-result -Wno-format-y2k
if IS_DARWIN
AM_CFLAGS += -Wno-deprecated-declarations -Wno-cast-align -Wno-macro-redefined
endif
AM_CPPFLAGS += -DDEBUG
endif
AM_CPPFLAGS += -iquote.
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
	cmd-display-panes.c \
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
	file.c \
	format.c \
	format-draw.c \
	grid-reader.c \
	grid-view.c \
	grid.c \
	hyperlinks.c \
	input-keys.c \
	input.c \
	job.c \
	key-bindings.c \
	key-string.c \
	layout-custom.c \
	layout-set.c \
	layout.c \
	log.c \
	menu.c \
	mode-tree.c \
	names.c \
	notify.c \
	options-table.c \
	options.c \
	paste.c \
	popup.c \
	proc.c \
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
	spawn.c \
	status.c \
	style.c \
	tmux.c \
	tmux.h \
	tmux-protocol.h \
	tty-acs.c \
	tty-features.c \
	tty-keys.c \
	tty-term.c \
	tty.c \
	utf8-combined.c \
	utf8.c \
	window-buffer.c \
	window-client.c \
	window-clock.c \
	window-copy.c \
	window-customize.c \
	window-tree.c \
	window.c \
	xmalloc.c \
	xmalloc.h \
	tmate.h \
	tmate-protocol.h \
	tmate-debug.c \
	tmate-decoder.c \
	tmate-encoder.c \
	tmate-env.c \
	tmate-msg.c \
	tmate-msgpack.c \
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
	cmd-display-panes.c \
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
	file.c \
	format.c \
	format-draw.c \
	grid-reader.c \
	grid-view.c \
	grid.c \
	hyperlinks.c \
	input-keys.c \
	input.c \
	job.c \
	key-bindings.c \
	key-string.c \
	layout-custom.c \
	layout-set.c \
	layout.c \
	log.c \
	menu.c \
	mode-tree.c \
	names.c \
	notify.c \
	options-table.c \
	options.c \
	paste.c \
	popup.c \
	proc.c \
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
	spawn.c \
	status.c \
	style.c \
	tmux.c \
	tmux.h \
	tmux-protocol.h \
	tty-acs.c \
	tty-features.c \
	tty-keys.c \
	tty-term.c \
	tty.c \
	utf8-combined.c \
	utf8.c \
	window-buffer.c \
	window-client.c \
	window-clock.c \
	window-copy.c \
	window-customize.c \
	window-tree.c \
	window.c \
	xmalloc.c \
	xmalloc.h \
	tmate.h \
	tmate-protocol.h \
	server/tmate.h \
	server/tmate-auth-keys.c \
	server/tmate-daemon-decoder.c \
	server/tmate-daemon-encoder.c \
	server/tmate-daemon-legacy.c \
	server/tmate-debug.c \
	server/tmate-main.c \
	server/tmate-msgpack.c \
	server/tmate-rand.c \
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

if NEED_FUZZING
check_PROGRAMS = fuzz/input-fuzzer
fuzz_input_fuzzer_LDFLAGS = $(FUZZING_LIBS)
fuzz_input_fuzzer_LDADD = $(LDADD) $(tmtv_OBJECTS)
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
