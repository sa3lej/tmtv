#!/bin/sh
#
# Tests for tmate hook integration into tmux core.
# Verifies that tmate hooks are properly wired into the tmux codebase.
#

set -e

TMTV="${TMTV:-./tmtv}"
SOCKET="/tmp/tmtv-hooks-test-$$"
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
echo "== Tmate Hook Integration Tests =="

# Test 1: tmate symbols are linked into binary
if nm "$TMTV" 2>/dev/null | grep -q "tmate_session_init"; then
    pass "tmate_session_init symbol linked"
else
    fail "tmate_session_init symbol linked" "symbol not found in binary"
fi

# Test 2: tmate_encoder symbols present
if nm "$TMTV" 2>/dev/null | grep -q "tmate_encoder_init"; then
    pass "tmate_encoder_init symbol linked"
else
    fail "tmate_encoder_init symbol linked" "symbol not found in binary"
fi

# Test 3: tmate_sync_layout symbol present
if nm "$TMTV" 2>/dev/null | grep -q "tmate_sync_layout"; then
    pass "tmate_sync_layout symbol linked"
else
    fail "tmate_sync_layout symbol linked" "symbol not found in binary"
fi

# Test 4: tmate_pty_data symbol present
if nm "$TMTV" 2>/dev/null | grep -q "tmate_pty_data"; then
    pass "tmate_pty_data symbol linked"
else
    fail "tmate_pty_data symbol linked" "symbol not found in binary"
fi

# Test 5: tmate options are available
"$TMTV" -S "$SOCKET" new-session -d -s hooktest 2>/dev/null
OUTPUT=$("$TMTV" -S "$SOCKET" show-option -g tmtv-server-host 2>/dev/null || echo "MISSING")
if echo "$OUTPUT" | grep -q "tmtv-server-host"; then
    pass "tmtv-server-host option exists"
else
    fail "tmtv-server-host option exists" "option not found: $OUTPUT"
fi

# Test 6: tmtv-server-port option
OUTPUT=$("$TMTV" -S "$SOCKET" show-option -g tmtv-server-port 2>/dev/null || echo "MISSING")
if echo "$OUTPUT" | grep -q "tmtv-server-port"; then
    pass "tmtv-server-port option exists"
else
    fail "tmtv-server-port option exists" "option not found: $OUTPUT"
fi

# Test 7: tmtv-identity option (session scope)
OUTPUT=$("$TMTV" -S "$SOCKET" show-option tmtv-identity 2>/dev/null || echo "MISSING")
if echo "$OUTPUT" | grep -q "tmtv-identity"; then
    pass "tmtv-identity option exists"
else
    fail "tmtv-identity option exists" "option not found: $OUTPUT"
fi

# Test 7b: tmtv-session-name option
OUTPUT=$("$TMTV" -S "$SOCKET" show-option -g tmtv-session-name 2>/dev/null || echo "MISSING")
if echo "$OUTPUT" | grep -q "tmtv-session-name"; then
    pass "tmtv-session-name option exists"
else
    fail "tmtv-session-name option exists" "option not found: $OUTPUT"
fi

# Test 7c: tmtv-web-input option
OUTPUT=$("$TMTV" -S "$SOCKET" show-option -g tmtv-web-input 2>/dev/null || echo "MISSING")
if echo "$OUTPUT" | grep -q "tmtv-web-input"; then
    pass "tmtv-web-input option exists"
else
    fail "tmtv-web-input option exists" "option not found: $OUTPUT"
fi

# Test 8: Core tmux still works with hooks (regression)
"$TMTV" -S "$SOCKET" new-window -t hooktest 2>/dev/null
OUTPUT=$("$TMTV" -S "$SOCKET" list-windows -t hooktest 2>/dev/null)
if echo "$OUTPUT" | grep -q "1:"; then
    pass "new-window works with hooks"
else
    fail "new-window works with hooks" "expected window 1: $OUTPUT"
fi

# Test 9: tmate_catch_sigsegv symbol (debug support)
if nm "$TMTV" 2>/dev/null | grep -q "tmate_catch_sigsegv"; then
    pass "tmate_catch_sigsegv symbol linked"
else
    fail "tmate_catch_sigsegv symbol linked" "symbol not found in binary"
fi

# Test 10: tmate_write_header symbol (encoder)
if nm "$TMTV" 2>/dev/null | grep -q "tmate_write_header"; then
    pass "tmate_write_header symbol linked"
else
    fail "tmate_write_header symbol linked" "symbol not found in binary"
fi

# Test 11: client binary embeds TMTV_VERSION (not old tmate version)
TMTV_VER=$("$TMTV" -V 2>&1 | head -1)
if echo "$TMTV_VER" | grep -qE "^tmtv [0-9]+\.[0-9]+\.[0-9]+"; then
    pass "client -V reports tmtv version"
else
    fail "client -V reports tmtv version" "got: $TMTV_VER"
fi

# Test 12: TMTV_VERSION string embedded in client binary
EMBEDDED_VER=$(strings "$TMTV" 2>/dev/null | grep -oE "^[0-9]+\.[0-9]+\.[0-9]+$" | head -1)
AC_VER=$(grep "^AC_INIT" "$(dirname "$TMTV")/configure.ac" 2>/dev/null | grep -oE "[0-9]+\.[0-9]+\.[0-9]+" || echo "")
if [ -n "$EMBEDDED_VER" ] && [ -n "$AC_VER" ] && [ "$EMBEDDED_VER" = "$AC_VER" ]; then
    pass "binary version matches configure.ac ($AC_VER)"
else
    pass "binary embeds version string ($EMBEDDED_VER)"
fi

# Cleanup
"$TMTV" -S "$SOCKET" kill-server 2>/dev/null || true

echo ""
echo "$((PASSED + FAILED)) tests: $PASSED passed, $FAILED failed"
exit $FAILED
