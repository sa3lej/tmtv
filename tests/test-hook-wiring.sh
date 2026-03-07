#!/bin/sh
#
# Verify tmate hooks are wired into core tmux source files.
# This checks that the tmate integration points exist at the source level.
#

set -e

SRCDIR="${SRCDIR:-..}"
PASSED=0
FAILED=0

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
echo "== Hook Wiring Tests (source level) =="

# Check that core files include tmate.h
for f in server.c cfg.c session.c window.c server-client.c; do
    if grep -q '#include "tmate.h"' "$SRCDIR/$f" 2>/dev/null; then
        pass "$f includes tmate.h"
    else
        fail "$f includes tmate.h" "missing #include \"tmate.h\""
    fi
done

# Check server.c has tmate_session_init call
if grep -q 'tmate_session_init' "$SRCDIR/server.c" 2>/dev/null; then
    pass "server.c calls tmate_session_init"
else
    fail "server.c calls tmate_session_init" "missing tmate_session_init call"
fi

# Check cfg.c has tmate_session_start call
if grep -q 'tmate_session_start' "$SRCDIR/cfg.c" 2>/dev/null; then
    pass "cfg.c calls tmate_session_start"
else
    fail "cfg.c calls tmate_session_start" "missing tmate_session_start call"
fi

# Check session.c has tmate_sync_layout calls
if grep -q 'tmate_sync_layout' "$SRCDIR/session.c" 2>/dev/null; then
    pass "session.c calls tmate_sync_layout"
else
    fail "session.c calls tmate_sync_layout" "missing tmate_sync_layout call"
fi

# Check window.c has tmate_pty_data call
if grep -q 'tmate_pty_data' "$SRCDIR/window.c" 2>/dev/null; then
    pass "window.c calls tmate_pty_data"
else
    fail "window.c calls tmate_pty_data" "missing tmate_pty_data call"
fi

# Check window.c initializes tmate_off
if grep -q 'tmate_off' "$SRCDIR/window.c" 2>/dev/null; then
    pass "window.c initializes tmate_off"
else
    fail "window.c initializes tmate_off" "missing tmate_off initialization"
fi

# Check server-client.c has layout sync
if grep -q 'tmate_sync_layout\|tmate_should_sync_layout' "$SRCDIR/server-client.c" 2>/dev/null; then
    pass "server-client.c has layout sync"
else
    fail "server-client.c has layout sync" "missing layout sync hook"
fi

# Check session.c has multi-session restriction
if grep -q 'multi.*session.*not.*supported\|next_session_id != 0' "$SRCDIR/session.c" 2>/dev/null; then
    pass "session.c has multi-session restriction"
else
    fail "session.c has multi-session restriction" "missing restriction"
fi

# Check tmux.c has tmate_catch_sigsegv
if grep -q 'tmate_catch_sigsegv' "$SRCDIR/tmux.c" 2>/dev/null; then
    pass "tmux.c calls tmate_catch_sigsegv"
else
    fail "tmux.c calls tmate_catch_sigsegv" "missing tmate_catch_sigsegv call"
fi

# Check session.c has tmate_write_fin on destroy
if grep -q 'tmate_write_fin' "$SRCDIR/session.c" 2>/dev/null; then
    pass "session.c calls tmate_write_fin"
else
    fail "session.c calls tmate_write_fin" "missing tmate_write_fin call"
fi

echo ""
echo "$((PASSED + FAILED)) tests: $PASSED passed, $FAILED failed"
exit $FAILED
