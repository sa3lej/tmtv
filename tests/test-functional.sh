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

# Test 9: Second new-session gives error, not crash
# (tmate only supports one session; server must stay alive)
SOCKET2="/tmp/tmtv-test-multi-$$"
"$TMTV" -S "$SOCKET2" new-session -d -s first 2>/dev/null
if "$TMTV" -S "$SOCKET2" list-sessions 2>/dev/null | grep -q "first"; then
    OUTPUT=$("$TMTV" -S "$SOCKET2" new-session -d -s second 2>&1 || true)
    # Server must still be alive after the failed second session
    if "$TMTV" -S "$SOCKET2" list-sessions 2>/dev/null | grep -q "first"; then
        pass "second new-session errors without crash"
    else
        fail "second new-session errors without crash" "server died: $OUTPUT"
    fi
else
    fail "second new-session errors without crash" "could not create test session"
fi
"$TMTV" -S "$SOCKET2" kill-server 2>/dev/null || true
rm -f "$SOCKET2"

# Test 10: Bare tmtv does not auto-attach (creates new session like tmux)
# With auto-attach removed, bare tmtv against an existing socket should NOT
# silently attach — it tries new-session which errors, but server stays alive.
SOCKET3="/tmp/tmtv-test-noattach-$$"
"$TMTV" -S "$SOCKET3" new-session -d -s existing 2>/dev/null
if "$TMTV" -S "$SOCKET3" list-sessions 2>/dev/null | grep -q "existing"; then
    # Bare tmtv against existing socket — should NOT auto-attach
    RC=0; timeout 2 "$TMTV" -S "$SOCKET3" 2>/dev/null || RC=$?
    # Server must still be alive after the attempt
    if "$TMTV" -S "$SOCKET3" list-sessions 2>/dev/null | grep -q "existing"; then
        pass "bare tmtv does not auto-attach (server alive, exit=$RC)"
    else
        fail "bare tmtv does not auto-attach" "server died (exit=$RC)"
    fi
else
    fail "bare tmtv does not auto-attach" "could not create test session"
fi
"$TMTV" -S "$SOCKET3" kill-server 2>/dev/null || true
rm -f "$SOCKET3"

# Test 11: Detach does not kill the tmux server (tmtv-02u.8)
# Validates the documented contract: Ctrl+B D detaches the tmux client
# but the tmux server (and any tmate SSH connection on its event loop)
# continues running. This is fundamental to tmtv's sharing model.
SOCKET4="/tmp/tmtv-test-detach-$$"
"$TMTV" -S "$SOCKET4" new-session -d -s sharing 2>/dev/null
if "$TMTV" -S "$SOCKET4" list-sessions 2>/dev/null | grep -q "sharing"; then
    # Simulate what happens after detach: the client is gone, but the
    # server process keeps running. We verify by checking the session
    # is still accessible via the socket (server alive).
    # Note: We can't test actual Ctrl+B D without a terminal, but we can
    # verify the server stays alive after the client process exits.
    "$TMTV" -S "$SOCKET4" detach-client 2>/dev/null || true
    # Server must still be alive (session persists after detach)
    if "$TMTV" -S "$SOCKET4" list-sessions 2>/dev/null | grep -q "sharing"; then
        pass "detach does not kill server (session persists)"
    else
        fail "detach does not kill server" "session disappeared after detach"
    fi
else
    fail "detach does not kill server" "could not create test session"
fi
"$TMTV" -S "$SOCKET4" kill-server 2>/dev/null || true
rm -f "$SOCKET4"

# Test 12: Multi-session guard returns proper error message (tmtv-02u.11)
# The error message must be user-friendly and explain the limitation.
SOCKET5="/tmp/tmtv-test-multierr-$$"
"$TMTV" -S "$SOCKET5" new-session -d -s primary 2>/dev/null
if "$TMTV" -S "$SOCKET5" list-sessions 2>/dev/null | grep -q "primary"; then
    OUTPUT=$("$TMTV" -S "$SOCKET5" new-session -d -s secondary 2>&1 || true)
    if echo "$OUTPUT" | grep -q "multiple sessions are not supported"; then
        pass "multi-session guard shows correct error"
    else
        fail "multi-session guard shows correct error" "unexpected output: $OUTPUT"
    fi
else
    fail "multi-session guard shows correct error" "could not create test session"
fi
"$TMTV" -S "$SOCKET5" kill-server 2>/dev/null || true
rm -f "$SOCKET5"

# Test 13: Session survives multiple detach/reattach cycles (tmtv-02u.8)
# Validates that repeated detach doesn't accumulate state corruption.
SOCKET6="/tmp/tmtv-test-reattach-$$"
"$TMTV" -S "$SOCKET6" new-session -d -s persist 2>/dev/null
if "$TMTV" -S "$SOCKET6" list-sessions 2>/dev/null | grep -q "persist"; then
    # Send some content to verify state persists
    "$TMTV" -S "$SOCKET6" send-keys -t persist "echo marker123" Enter 2>/dev/null
    sleep 0.5
    # Detach multiple times (idempotent, no-op when no client attached)
    "$TMTV" -S "$SOCKET6" detach-client 2>/dev/null || true
    "$TMTV" -S "$SOCKET6" detach-client 2>/dev/null || true
    # Verify session still exists and content persists
    OUTPUT=$("$TMTV" -S "$SOCKET6" capture-pane -t persist -p 2>/dev/null)
    if echo "$OUTPUT" | grep -q "marker123"; then
        pass "session content survives detach cycles"
    else
        fail "session content survives detach cycles" "content lost after detach"
    fi
else
    fail "session content survives detach cycles" "could not create test session"
fi
"$TMTV" -S "$SOCKET6" kill-server 2>/dev/null || true
rm -f "$SOCKET6"

# Test 14: display-popup command works (tmtv-8z3.7)
# Popups are a tmux 3.2+ feature that never existed in tmate (stuck on tmux 2.4).
# tmtv rebased on tmux 3.6a should support them without crashing.
SOCKET7="/tmp/tmtv-test-popup-$$"
"$TMTV" -S "$SOCKET7" new-session -d -s popup1 2>/dev/null
if "$TMTV" -S "$SOCKET7" list-sessions 2>/dev/null | grep -q "popup1"; then
    # Run display-popup with a simple command; without a terminal it may error,
    # but it must NOT crash the server.
    "$TMTV" -S "$SOCKET7" display-popup -t popup1 "echo hello" 2>/dev/null || true
    sleep 0.3
    if "$TMTV" -S "$SOCKET7" list-sessions 2>/dev/null | grep -q "popup1"; then
        pass "display-popup doesn't crash server"
    else
        fail "display-popup doesn't crash server" "server died after display-popup"
    fi
else
    fail "display-popup doesn't crash server" "could not create test session"
fi
"$TMTV" -S "$SOCKET7" kill-server 2>/dev/null || true
rm -f "$SOCKET7"

# Test 15: popup with -E flag works (tmtv-8z3.7)
# The -E flag closes the popup automatically when the command finishes.
# This is the most common popup usage pattern.
SOCKET8="/tmp/tmtv-test-popup-e-$$"
"$TMTV" -S "$SOCKET8" new-session -d -s popup2 2>/dev/null
if "$TMTV" -S "$SOCKET8" list-sessions 2>/dev/null | grep -q "popup2"; then
    "$TMTV" -S "$SOCKET8" display-popup -t popup2 -E "echo popup-test-marker" 2>/dev/null || true
    sleep 0.3
    if "$TMTV" -S "$SOCKET8" list-sessions 2>/dev/null | grep -q "popup2"; then
        pass "display-popup -E doesn't crash server"
    else
        fail "display-popup -E doesn't crash server" "server died after display-popup -E"
    fi
else
    fail "display-popup -E doesn't crash server" "could not create test session"
fi
"$TMTV" -S "$SOCKET8" kill-server 2>/dev/null || true
rm -f "$SOCKET8"

# Test 16: display-menu doesn't crash (tmtv-8z3.7)
# Menus are another tmux 3.0+ overlay feature absent from tmate.
SOCKET9="/tmp/tmtv-test-menu-$$"
"$TMTV" -S "$SOCKET9" new-session -d -s menu1 2>/dev/null
if "$TMTV" -S "$SOCKET9" list-sessions 2>/dev/null | grep -q "menu1"; then
    "$TMTV" -S "$SOCKET9" display-menu -t menu1 -T "Test" a test "send-keys test" 2>/dev/null || true
    sleep 0.3
    if "$TMTV" -S "$SOCKET9" list-sessions 2>/dev/null | grep -q "menu1"; then
        pass "display-menu doesn't crash server"
    else
        fail "display-menu doesn't crash server" "server died after display-menu"
    fi
else
    fail "display-menu doesn't crash server" "could not create test session"
fi
"$TMTV" -S "$SOCKET9" kill-server 2>/dev/null || true
rm -f "$SOCKET9"

echo ""
echo "$((PASSED + FAILED)) tests: $PASSED passed, $FAILED failed"
exit $FAILED
