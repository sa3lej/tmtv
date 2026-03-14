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

UNIT_TESTS = test-msgpack test-version-compare test-tmate-env test-find-session test-input-socket
SERVER_TESTS = test-server-header test-session-tmate-ptr test-sse-mux test-server-format
TMTV ?= ../tmtv

.PHONY: all unit server-unit functional hooks session-cleanup integration clean

all: unit server-unit functional hooks session-cleanup

unit: $(UNIT_TESTS)
	@echo ""
	@echo "Running unit tests..."
	@for t in $(UNIT_TESTS); do ./$$t || exit 1; done

server-unit: $(SERVER_TESTS)
	@echo ""
	@echo "Running server unit tests..."
	@for t in $(SERVER_TESTS); do ./$$t || exit 1; done

test-msgpack: test-msgpack.c test-harness.h
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
UTF8PROC_CFLAGS := $(shell pkg-config --cflags libutf8proc 2>/dev/null)

test-server-header: test-server-header.c test-harness.h ../server/tmate.h ../server/tmate-protocol.h
	$(CC) $(CFLAGS) $(PARENT_DEFS) $(PARENT_CPPFLAGS) $(MSGPACK_CFLAGS) $(LIBSSH_CFLAGS) $(LIBEVENT_CFLAGS) $(UTF8PROC_CFLAGS) -I.. -o $@ $< $(LDFLAGS)

test-session-tmate-ptr: test-session-tmate-ptr.c test-harness.h ../tmux.h ../tmate.h
	$(CC) $(CFLAGS) $(PARENT_DEFS) $(PARENT_CPPFLAGS) $(MSGPACK_CFLAGS) $(LIBSSH_CFLAGS) $(LIBEVENT_CFLAGS) $(UTF8PROC_CFLAGS) -I.. -o $@ $< $(LDFLAGS)

test-sse-mux: test-sse-mux.c test-harness.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

LIBBSD := $(shell [ "$$(uname -s)" = Linux ] && echo -lbsd)

test-server-format: test-server-format.c test-harness.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LIBBSD)

functional:
	@echo ""
	@echo "Running functional tests..."
	@TMTV=$(TMTV) sh test-functional.sh

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
