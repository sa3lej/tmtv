# Tests Makefile for tmtv
#
# Usage:
#   make              - build and run all tests
#   make unit         - build and run unit tests only
#   make functional   - run functional tests only
#   make clean        - clean test binaries

CC ?= gcc
CFLAGS ?= -std=gnu99 -Wall -Wextra -O2
LDFLAGS ?=

# Find msgpack via pkg-config
MSGPACK_CFLAGS := $(shell pkg-config --cflags msgpack-c 2>/dev/null || pkg-config --cflags msgpack 2>/dev/null)
MSGPACK_LIBS := $(shell pkg-config --libs msgpack-c 2>/dev/null || pkg-config --libs msgpack 2>/dev/null)

LIBSSH_CFLAGS := $(shell pkg-config --cflags libssh 2>/dev/null)
LIBEVENT_CFLAGS := $(shell pkg-config --cflags libevent 2>/dev/null)

UNIT_TESTS = test-msgpack test-version-compare test-tmate-env test-find-session test-input-socket test-hardening test-compat test-json test-proc
SERVER_TESTS = test-server-header test-session-tmate-ptr test-sse-mux test-server-format test-pty-pending test-resume
TMTV ?= ../tmtv
PYTHON ?= python3
GC_SECTIONS := $(shell if [ "$$(uname -s)" = Darwin ]; then echo -Wl,-dead_strip; else echo -Wl,--gc-sections; fi)

.PHONY: all unit server-unit functional hooks session-cleanup prompt-history child-environment integration clean

# Explicit opt-in: disposable offline Docker relay, never the installed service.
.PHONY: relay-e2e
relay-e2e:
	$(MAKE) -C .. tmtv tmtv-server
	$(PYTHON) run-relay-e2e.py $(E2E_ARGS)

# Same isolated production fixture, with an actual browser instead of pyte.
.PHONY: relay-browser-e2e
relay-browser-e2e:
	$(MAKE) -C .. tmtv tmtv-server
	$(PYTHON) run-relay-e2e.py --browser $(E2E_ARGS)

# Portable production-host checks; no Linux relay objects or jail required.
.PHONY: host
host: unit core38-commands mode-lifecycle functional hooks prompt-history child-environment list-keys list-commands pane-lifetime event-payload events cmd-queue options-hooks monitor control-subscriptions

all: host relay-copy-output server-unit session-cleanup installer

# Offline installer checks; real sha256sum and shasum tools, no live installs.
.PHONY: installer
installer:
	$(PYTHON) test-installer.py

unit: $(UNIT_TESTS)
	@echo ""
	@echo "Running unit tests..."
	@for t in $(UNIT_TESTS); do ./$$t || exit 1; done

server-unit: $(SERVER_TESTS)
	@echo ""
	@echo "Running server unit tests..."
	@for t in $(SERVER_TESTS); do ./$$t || exit 1; done

# Compile the production fallbacks even on platforms with native functions.
test-compat: test-compat.c test-harness.h ../compat/base64.c ../compat/getline.c ../compat/getpeereid.c ../compat.h ../tmux.h
	$(CC) $(CFLAGS) $(PARENT_DEFS) $(PARENT_CPPFLAGS) $(MSGPACK_CFLAGS) $(LIBSSH_CFLAGS) $(LIBEVENT_CFLAGS) $(UTF8PROC_CFLAGS) -I.. -o $@ $< $(LDFLAGS)

# Link the same parser and allocation implementations used by host and relay.
test-json: test-json.c test-harness.h ../json.c ../xmalloc.c ../xmalloc.h ../compat/recallocarray.c ../compat/reallocarray.c ../compat/explicit_bzero.c ../compat/freezero.c ../tmux.h
	$(CC) $(CFLAGS) $(PARENT_DEFS) $(PARENT_CPPFLAGS) $(MSGPACK_CFLAGS) $(LIBSSH_CFLAGS) $(LIBEVENT_CFLAGS) -I.. -o $@ test-json.c ../json.c ../xmalloc.c ../compat/recallocarray.c $(ALLOC_COMPAT_SOURCES) $(LDFLAGS) $(shell pkg-config --libs libevent) $(LIBBSD)

# Compile actual peer/libevent/imsg paths; discard unused process startup code.
test-proc: test-proc.c test-harness.h ../proc.c ../tmux.h ../compat/imsg.c ../compat/imsg-buffer.c ../compat/getpeereid.c ../compat/reallocarray.c ../compat/explicit_bzero.c ../compat/freezero.c ../xmalloc.c ../log.c
	$(CC) $(CFLAGS) $(PARENT_DEFS) $(PARENT_CPPFLAGS) $(MSGPACK_CFLAGS) $(LIBSSH_CFLAGS) $(LIBEVENT_CFLAGS) $(UTF8PROC_CFLAGS) -I.. -ffunction-sections -fdata-sections -o $@ test-proc.c ../compat/imsg.c ../compat/imsg-buffer.c $(PEER_COMPAT_SOURCE) ../compat/recallocarray.c ../xmalloc.c ../log.c $(ALLOC_COMPAT_SOURCES) $(LDFLAGS) $(GC_SECTIONS) $(shell pkg-config --libs libevent) $(LIBBSD)

test-msgpack: test-msgpack.c test-harness.h
	$(CC) $(CFLAGS) $(MSGPACK_CFLAGS) -o $@ $< $(LDFLAGS) $(MSGPACK_LIBS)

test-hardening: test-hardening.c test-harness.h
	$(CC) $(CFLAGS) $(MSGPACK_CFLAGS) -o $@ $< $(LDFLAGS) $(MSGPACK_LIBS)

test-version-compare: test-version-compare.c test-harness.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test-tmate-env: test-tmate-env.c test-harness.h ../tmate-env.c ../tmate.h
	$(CC) $(CFLAGS) $(PARENT_DEFS) $(PARENT_CPPFLAGS) $(MSGPACK_CFLAGS) $(LIBSSH_CFLAGS) $(LIBEVENT_CFLAGS) $(UTF8PROC_CFLAGS) -I.. -include ../tmate-env.c -o $@ $< $(LDFLAGS)

test-find-session: test-find-session.c test-harness.h ../tmux.h ../tmate.h
	$(CC) $(CFLAGS) $(PARENT_DEFS) $(PARENT_CPPFLAGS) $(MSGPACK_CFLAGS) $(LIBSSH_CFLAGS) $(LIBEVENT_CFLAGS) $(UTF8PROC_CFLAGS) -I.. -o $@ $< $(LDFLAGS)

test-input-socket: test-input-socket.c test-harness.h
	$(CC) $(CFLAGS) $(MSGPACK_CFLAGS) -o $@ $< $(LDFLAGS) $(MSGPACK_LIBS)

# Extract DEFS and CPPFLAGS from parent Makefile (configure-generated flags)
PARENT_DEFS := $(shell $(MAKE) -C .. -n -p 2>/dev/null | grep '^DEFS = ' | head -1 | sed 's/^DEFS = //')
PARENT_CPPFLAGS := $(shell $(MAKE) -C .. -n -p 2>/dev/null | grep '^CPPFLAGS = ' | head -1 | sed 's/^CPPFLAGS = //')
PARENT_LDFLAGS := $(shell $(MAKE) -C .. -n -p 2>/dev/null | grep '^LDFLAGS = ' | head -1 | sed 's/^LDFLAGS = //')
UTF8PROC_CFLAGS := $(shell pkg-config --cflags libutf8proc 2>/dev/null)
# Standalone allocator/parser tests need the same Darwin fallback functions as
# the host. On libc platforms that provide them, avoid duplicate definitions.
ALLOC_COMPAT_SOURCES = $(if $(findstring -DHAVE_REALLOCARRAY=1,$(PARENT_DEFS)),,../compat/reallocarray.c) $(if $(findstring -DHAVE_EXPLICIT_BZERO=1,$(PARENT_DEFS)),,../compat/explicit_bzero.c) $(if $(findstring -DHAVE_FREEZERO=1,$(PARENT_DEFS)),,../compat/freezero.c)
# Do not override Darwin's real credential checks with an unused fallback.
PEER_COMPAT_SOURCE = $(if $(findstring -DHAVE_GETPEEREID=1,$(PARENT_DEFS)),,../compat/getpeereid.c)


test-server-header: test-server-header.c test-harness.h ../server/tmate.h ../server/tmate-protocol.h
	$(CC) $(CFLAGS) $(PARENT_DEFS) $(PARENT_CPPFLAGS) $(MSGPACK_CFLAGS) $(LIBSSH_CFLAGS) $(LIBEVENT_CFLAGS) $(UTF8PROC_CFLAGS) -I.. -o $@ $< $(LDFLAGS)

test-session-tmate-ptr: test-session-tmate-ptr.c test-harness.h ../tmux.h ../tmate.h
	$(CC) $(CFLAGS) $(PARENT_DEFS) $(PARENT_CPPFLAGS) $(MSGPACK_CFLAGS) $(LIBSSH_CFLAGS) $(LIBEVENT_CFLAGS) $(UTF8PROC_CFLAGS) -I.. -o $@ $< $(LDFLAGS)

test-sse-mux: test-sse-mux.c test-harness.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

LIBBSD := $(shell [ "$$(uname -s)" = Linux ] && echo -lbsd)

test-server-format: test-server-format.c test-harness.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBBSD)

test-pty-pending: test-pty-pending.c test-harness.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test-resume: test-resume.c test-harness.h ../server/tmate-resume.c ../server/tmate-resume.h
	$(CC) $(CFLAGS) $(PARENT_CPPFLAGS) $(shell pkg-config --cflags libcrypto) -o $@ test-resume.c ../server/tmate-resume.c $(PARENT_LDFLAGS) $(LDFLAGS) $(shell pkg-config --libs libcrypto)

functional:
	@echo ""
	@echo "Running functional tests..."
	@TMTV=$(TMTV) sh test-functional.sh

prompt-history:
	@TMTV=$(TMTV) python3 test-prompt-history.py

list-keys:
	@TMTV=$(TMTV) python3 test-list-keys.py

list-commands:
	@TMTV=$(TMTV) python3 test-list-commands.py

child-environment:
	@TMTV=$(TMTV) python3 test-child-environment.py

hooks:
	@echo ""
	@echo "Running tmate hook integration tests..."
	@TMTV=$(TMTV) sh test-tmate-hooks.sh

session-cleanup:
	@echo ""
	@echo "Running session cleanup tests..."
	@sh test-session-cleanup.sh

integration:
	@echo ""
	@echo "Running integration tests (requires TEST_HOST env var)..."
	@test -n "$(TEST_HOST)" || { echo "ERROR: set TEST_HOST=<ip>"; exit 1; }
	@TEST_HOST=$(TEST_HOST) sh test-integration.sh

wiring:
	@echo ""
	@echo "Running hook wiring tests..."
	@SRCDIR=$(SRCDIR) sh test-hook-wiring.sh

clean:
	rm -f $(UNIT_TESTS) $(SERVER_TESTS)

.PHONY: pane-lifetime
pane-lifetime:
	$(MAKE) -C .. tests/test-window-lifetime
	./test-window-lifetime

.PHONY: event-payload
event-payload:
	$(MAKE) -C .. tests/test-event-payload
	./test-event-payload

.PHONY: events
events:
	$(MAKE) -C .. tests/test-events
	./test-events

.PHONY: cmd-queue
cmd-queue:
	$(MAKE) -C .. tests/test-cmd-queue
	./test-cmd-queue

.PHONY: options-hooks
options-hooks:
	$(MAKE) -C .. tests/test-options-hooks
	./test-options-hooks

.PHONY: monitor
monitor:
	$(MAKE) -C .. tests/test-monitor
	./test-monitor

.PHONY: control-subscriptions
control-subscriptions:
	$(MAKE) -C .. tests/test-control-subscriptions
	./test-control-subscriptions

.PHONY: list-keys list-commands

.PHONY: mode-lifecycle
mode-lifecycle:
	$(MAKE) -C .. tests/test-mode-lifecycle
	./test-mode-lifecycle

.PHONY: relay-copy-output
relay-copy-output:
	$(MAKE) -C .. tests/test-relay-copy-output tests/test-mode-lifecycle
	./test-relay-copy-output
	@set -eu; wire=$$(mktemp); trap 'rm -f "$$wire"' EXIT HUP INT TERM; \
	./test-mode-lifecycle --emit-pane-snapshot > "$$wire"; \
	    ./test-relay-copy-output --host-pane-codec < "$$wire"; \
	    ./test-relay-copy-output --host-pane-codec-bulk < "$$wire"; \
	    ./test-mode-lifecycle --emit-grid-snapshot > "$$wire"; \
	    ./test-relay-copy-output --host-grid-codec < "$$wire"; \
	    ./test-relay-copy-output --host-grid-codec-bulk < "$$wire"; \
	    ./test-mode-lifecycle --emit-copy-output > "$$wire"; \
	    ./test-relay-copy-output --host-output < "$$wire"; \
	    ./test-mode-lifecycle --emit-copy-exit > "$$wire"; \
	    ./test-relay-copy-output --host-exit < "$$wire"; \
	    ./test-mode-lifecycle --emit-copy-state > "$$wire"; \
	    ./test-relay-copy-output --host-copy-state < "$$wire"; \
	    ./test-relay-copy-output --host-copy-state-bulk < "$$wire"; \
	    ./test-mode-lifecycle --emit-frozen-copy > "$$wire"; \
	    ./test-relay-copy-output --host-frozen-copy < "$$wire"; \
	    ./test-relay-copy-output --host-frozen-copy-bulk < "$$wire"; \
	    ./test-mode-lifecycle --emit-snapshot > "$$wire"; \
	    ./test-relay-copy-output --host-snapshot < "$$wire"; \
	    ./test-relay-copy-output --host-snapshot-bulk < "$$wire"; \
	    ./test-mode-lifecycle --emit-layout-snapshot > "$$wire"; \
	    ./test-relay-copy-output --host-layout-snapshot < "$$wire"; \
	    ./test-relay-copy-output --host-layout-snapshot-bulk < "$$wire"; \
	    for axis in horizontal vertical mixed-horizontal mixed-vertical; do \
	        ./test-mode-lifecycle --emit-$$axis-reconnect > "$$wire"; \
	        ./test-relay-copy-output --host-$$axis-reconnect < "$$wire"; \
	        ./test-relay-copy-output --host-$$axis-reconnect-bulk < "$$wire"; \
	    done

.PHONY: core38-commands
core38-commands:
	@TMTV=$(TMTV) $(PYTHON) test-core38-commands.py
