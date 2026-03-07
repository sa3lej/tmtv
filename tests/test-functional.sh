#!/bin/sh
#
# Functional tests for tmtv.
# Tests basic tmux functionality works after the tmate port.
#

set -e

TMTV="${TMTV:-./tmtv}"
SOCKET="/tmp/tmtv-test-$$"
PASSED=0
FAILED=0

cleanup() {
    "$TMTV" -S "$SOCKET" kill-server 2>/dev/null || true
    rm -f "$SOCKET"
}
trap cleanup EXIT

pass() {
    PASSED=$((PASSED + 1))
    printf "  %-50s PASS\n" "$1"
}

fail() {
    FAILED=$((FAILED + 1))
    printf "  %-50s FAIL\n" "$1"
    echo "    $2" >&2
}

echo ""
echo "== Functional Tests =="

# Test 1: Binary exists and runs
if "$TMTV" -V >/dev/null 2>&1; then
    pass "binary runs"
else
    fail "binary runs" "$TMTV -V failed"
fi

# Test 2: Can start a server
"$TMTV" -S "$SOCKET" new-session -d -s test 2>/dev/null
if [ $? -eq 0 ]; then
    pass "start server"
else
    fail "start server" "new-session failed"
fi

# Test 3: Can list sessions
OUTPUT=$("$TMTV" -S "$SOCKET" list-sessions 2>/dev/null)
if echo "$OUTPUT" | grep -q "test"; then
    pass "list sessions"
else
    fail "list sessions" "expected 'test' in output: $OUTPUT"
fi

# Test 4: Can create a new window
"$TMTV" -S "$SOCKET" new-window -t test 2>/dev/null
OUTPUT=$("$TMTV" -S "$SOCKET" list-windows -t test 2>/dev/null)
if echo "$OUTPUT" | grep -q "1:"; then
    pass "create window"
else
    fail "create window" "expected window 1 in output: $OUTPUT"
fi

# Test 5: Can send keys
"$TMTV" -S "$SOCKET" send-keys -t test "echo hello" Enter 2>/dev/null
sleep 0.5
OUTPUT=$("$TMTV" -S "$SOCKET" capture-pane -t test -p 2>/dev/null)
if echo "$OUTPUT" | grep -q "hello"; then
    pass "send keys"
else
    fail "send keys" "expected 'hello' in output"
fi

# Test 6: Can split pane
"$TMTV" -S "$SOCKET" split-window -t test 2>/dev/null
OUTPUT=$("$TMTV" -S "$SOCKET" list-panes -t test 2>/dev/null | wc -l)
if [ "$OUTPUT" -ge 2 ]; then
    pass "split pane"
else
    fail "split pane" "expected >= 2 panes, got $OUTPUT"
fi

# Test 7: Can set and get options
"$TMTV" -S "$SOCKET" set-option -g status-interval 5 2>/dev/null
OUTPUT=$("$TMTV" -S "$SOCKET" show-option -g status-interval 2>/dev/null)
if echo "$OUTPUT" | grep -q "5"; then
    pass "set/get option"
else
    fail "set/get option" "expected '5' in output: $OUTPUT"
fi

# Test 8: Can kill session
"$TMTV" -S "$SOCKET" kill-session -t test 2>/dev/null
OUTPUT=$("$TMTV" -S "$SOCKET" list-sessions 2>/dev/null || echo "no sessions")
if echo "$OUTPUT" | grep -q "no sessions\|no server"; then
    pass "kill session"
else
    fail "kill session" "session still exists: $OUTPUT"
fi

echo ""
echo "$((PASSED + FAILED)) tests: $PASSED passed, $FAILED failed"
exit $FAILED
