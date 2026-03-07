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

UNIT_TESTS = test-msgpack
TMTV ?= ../tmtv

.PHONY: all unit functional clean

all: unit functional

unit: $(UNIT_TESTS)
	@echo ""
	@echo "Running unit tests..."
	@for t in $(UNIT_TESTS); do ./$$t || exit 1; done

test-msgpack: test-msgpack.c test-harness.h
	$(CC) $(CFLAGS) $(MSGPACK_CFLAGS) -o $@ $< $(LDFLAGS) $(MSGPACK_LIBS)

functional:
	@echo ""
	@echo "Running functional tests..."
	@TMTV=$(TMTV) sh test-functional.sh

clean:
	rm -f $(UNIT_TESTS)
