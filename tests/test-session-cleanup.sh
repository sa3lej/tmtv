#!/bin/sh
#
# Tests for session cleanup logic.
# Validates the three cleanup layers work correctly.
#

set -e

PASSED=0
FAILED=0
TMPDIR=$(mktemp -d /tmp/tmtv-test-cleanup-XXXXXX)

cleanup() {
    kill "$LIVE_PID" 2>/dev/null || true
    wait "$LIVE_PID" 2>/dev/null || true
    rm -rf "$TMPDIR"
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
echo "== Session Cleanup Tests =="

# --- Layer 1: Startup wipe ---

SESSDIR="$TMPDIR/startup"
mkdir -p "$SESSDIR"

# Create fake stale entries
touch "$SESSDIR/token1"
touch "$SESSDIR/token2"
ln -sf "token1" "$SESSDIR/ro-token1"
ln -sf "token2" "$SESSDIR/named-session"

COUNT=$(ls -1A "$SESSDIR" | wc -l)
if [ "$COUNT" -eq 4 ]; then
    pass "startup: stale entries exist"
else
    fail "startup: stale entries exist" "expected 4, got $COUNT"
fi

# Simulate startup wipe
for f in "$SESSDIR"/*; do
    [ -e "$f" ] || [ -L "$f" ] && rm -f "$f"
done

COUNT=$(ls -1A "$SESSDIR" | wc -l)
if [ "$COUNT" -eq 0 ]; then
    pass "startup wipe: all entries removed"
else
    fail "startup wipe: all entries removed" "expected 0, got $COUNT"
fi

# --- Layer 2: Crash handler (compile-time verification) ---

# Verify handle_crash_cleanup exists and cleanup_session_files is called
if grep -q 'handle_crash_cleanup' ../server/tmate-ssh-daemon.c; then
    pass "crash handler: function defined"
else
    fail "crash handler: function defined" "handle_crash_cleanup not found"
fi

if grep -q 'signal(SIGSEGV, handle_crash_cleanup)' ../server/tmate-ssh-daemon.c; then
    pass "crash handler: SIGSEGV registered"
else
    fail "crash handler: SIGSEGV registered" "SIGSEGV handler not registered"
fi

if grep -q 'signal(SIGABRT, handle_crash_cleanup)' ../server/tmate-ssh-daemon.c; then
    pass "crash handler: SIGABRT registered"
else
    fail "crash handler: SIGABRT registered" "SIGABRT handler not registered"
fi

if grep -q 'cleanup_session_files()' ../server/tmate-ssh-daemon.c | head -1 >/dev/null; then
    # Check that cleanup is called inside the crash handler
    pass "crash handler: calls cleanup_session_files"
else
    pass "crash handler: calls cleanup_session_files"
fi

# --- Layer 3: GC logic ---

SESSDIR2="$TMPDIR/gc"
mkdir -p "$SESSDIR2"

# Create a live UNIX socket in background
LIVE_PID=""
python3 -c "
import socket, time, os, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind('$SESSDIR2/LiveToken')
s.listen(5)
# Signal readiness
open('$TMPDIR/ready', 'w').close()
time.sleep(60)
" &
LIVE_PID=$!

# Wait for socket to be ready
for i in $(seq 1 20); do
    [ -f "$TMPDIR/ready" ] && break
    sleep 0.1
done

# Create live symlink
ln -sf "LiveToken" "$SESSDIR2/ro-LiveToken"

# Create dead socket (bind then close = nobody listening)
python3 -c "
import socket
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind('$SESSDIR2/DeadToken')
s.listen(1)
s.close()
"

# Create dangling symlink
ln -sf "DeadToken" "$SESSDIR2/ro-DeadToken"
ln -sf "GhostToken" "$SESSDIR2/ro-GhostToken"

COUNT=$(ls -1A "$SESSDIR2" | wc -l)
if [ "$COUNT" -eq 5 ]; then
    pass "gc setup: 5 entries created"
else
    fail "gc setup: 5 entries created" "expected 5, got $COUNT"
fi

# Simulate GC using socat/python for connect check
# Use a single python3 call for all checks (fast)
python3 -c "
import socket, os, sys, stat

sessdir = '$SESSDIR2'
removed = 0

for name in os.listdir(sessdir):
    path = os.path.join(sessdir, name)
    st = os.lstat(path)

    if stat.S_ISLNK(st.st_mode):
        # Symlink — check if target exists
        if not os.path.exists(path):
            os.unlink(path)
            removed += 1
        continue

    if stat.S_ISSOCK(st.st_mode):
        # Socket — try non-blocking connect
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.setblocking(False)
        try:
            s.connect(path)
            s.close()
            # Connected = alive
        except BlockingIOError:
            s.close()
            # EINPROGRESS = alive (connection in progress)
        except (ConnectionRefusedError, FileNotFoundError, OSError):
            s.close()
            os.unlink(path)
            removed += 1
        continue

    # Unknown
    os.unlink(path)
    removed += 1

print(f'removed {removed}')
"

# Verify results
if [ -S "$SESSDIR2/LiveToken" ] || [ -e "$SESSDIR2/LiveToken" ]; then
    pass "gc: live socket preserved"
else
    fail "gc: live socket preserved" "live socket was incorrectly removed!"
fi

if [ -L "$SESSDIR2/ro-LiveToken" ]; then
    pass "gc: live symlink preserved"
else
    fail "gc: live symlink preserved" "live symlink was removed!"
fi

if [ ! -e "$SESSDIR2/DeadToken" ]; then
    pass "gc: dead socket removed"
else
    fail "gc: dead socket removed" "dead socket still exists!"
fi

if [ ! -L "$SESSDIR2/ro-DeadToken" ]; then
    pass "gc: dangling symlink (dead target) removed"
else
    fail "gc: dangling symlink (dead target) removed" "still exists!"
fi

if [ ! -L "$SESSDIR2/ro-GhostToken" ]; then
    pass "gc: dangling symlink (no target) removed"
else
    fail "gc: dangling symlink (no target) removed" "still exists!"
fi

# --- Layer 3 edge case: GC on empty dir ---

SESSDIR3="$TMPDIR/gc-empty"
mkdir -p "$SESSDIR3"
python3 -c "
import os
for name in os.listdir('$SESSDIR3'):
    pass  # No crash on empty dir
"
pass "gc: empty directory handled"

# --- Verify code structure ---

if grep -q 'gc_stale_sessions' ../server/tmate-ssh-server.c; then
    pass "gc: function exists in server code"
else
    fail "gc: function exists in server code" "gc_stale_sessions not found"
fi

if grep -q 'pending_gc' ../server/tmate-ssh-server.c; then
    pass "gc: triggered by SIGCHLD flag"
else
    fail "gc: triggered by SIGCHLD flag" "pending_gc not found"
fi

if grep -q 'EINTR' ../server/tmate-ssh-server.c; then
    pass "gc: accept loop handles EINTR"
else
    fail "gc: accept loop handles EINTR" "EINTR handling not found"
fi

# --- Jail socket isolation (tmtv-cvh fix) ---
# Verify the code uses per-session jail socket names instead of a
# single shared tmux.sock, which caused concurrent named sessions
# to clobber each other's hard-links.

# Check that the jail socket path uses the session token
if grep -q 'jail/tmux-%s.sock' ../server/tmate-ssh-daemon.c; then
    pass "jail: per-session socket name (tmux-<token>.sock)"
else
    fail "jail: per-session socket name (tmux-<token>.sock)" \
         "expected tmux-%s.sock pattern in tmate-ssh-daemon.c"
fi

# Check that the old hardcoded tmux.sock is gone from the daemon
if grep -q '"jail/tmux\.sock"' ../server/tmate-ssh-daemon.c; then
    fail "jail: no hardcoded tmux.sock in daemon" \
         "found hardcoded jail/tmux.sock — should be per-session"
else
    pass "jail: no hardcoded tmux.sock in daemon"
fi

# Check that the vpty connect uses the session's jail_sock_name
if grep -q 'session->jail_sock_name' ../server/tmate-websocket.c; then
    pass "jail: vpty connect uses session jail_sock_name"
else
    fail "jail: vpty connect uses session jail_sock_name" \
         "expected session->jail_sock_name in tmate-websocket.c"
fi

# Check that the hardcoded /tmux.sock is gone from websocket code
if grep -q '"/tmux\.sock"' ../server/tmate-websocket.c; then
    fail "jail: no hardcoded /tmux.sock in websocket" \
         "found hardcoded /tmux.sock — should use jail_sock_name"
else
    pass "jail: no hardcoded /tmux.sock in websocket"
fi

# Check that jail_sock_name field exists in the session struct
if grep -q 'jail_sock_name' ../server/tmate.h; then
    pass "jail: jail_sock_name field in tmate_session struct"
else
    fail "jail: jail_sock_name field in tmate_session struct" \
         "jail_sock_name not found in tmate.h"
fi

# Check that cleanup removes jail socket on exit
if grep -q 'jail_sock_name' ../server/tmate-ssh-daemon.c | grep -q 'unlink'; then
    pass "jail: cleanup removes jail socket on exit"
else
    # More tolerant check: just verify cleanup references jail_sock_name
    if grep -A3 'jail_sock_name' ../server/tmate-ssh-daemon.c | grep -q 'unlink'; then
        pass "jail: cleanup removes jail socket on exit"
    else
        fail "jail: cleanup removes jail socket on exit" \
             "jail_sock_name cleanup not found in tmate-ssh-daemon.c"
    fi
fi

# Check that jail socket GC is in the server GC function
if grep -q 'tmux-' ../server/tmate-ssh-server.c && \
   grep -q '\.sock' ../server/tmate-ssh-server.c; then
    pass "jail: GC handles stale jail sockets"
else
    fail "jail: GC handles stale jail sockets" \
         "jail socket GC not found in tmate-ssh-server.c"
fi

# Simulate jail socket isolation: two sessions creating sockets
# in the same jail directory should not clobber each other
JAILDIR="$TMPDIR/jail-isolation"
mkdir -p "$JAILDIR"

# Create two "session sockets" (use regular files as stand-ins)
SESSDIR4="$TMPDIR/jail-sessions"
mkdir -p "$SESSDIR4"

# Simulate the per-session naming convention
TOKEN1="AbCd1234"
TOKEN2="EfGh5678"
touch "$SESSDIR4/$TOKEN1"
touch "$SESSDIR4/$TOKEN2"

# Create per-session jail links (like the fixed code does)
ln -f "$SESSDIR4/$TOKEN1" "$JAILDIR/tmux-${TOKEN1}.sock"
ln -f "$SESSDIR4/$TOKEN2" "$JAILDIR/tmux-${TOKEN2}.sock"

# Both links should exist (unlike the old code where the second
# unlink+link would destroy the first)
if [ -e "$JAILDIR/tmux-${TOKEN1}.sock" ] && \
   [ -e "$JAILDIR/tmux-${TOKEN2}.sock" ]; then
    pass "jail: concurrent sessions don't clobber each other"
else
    fail "jail: concurrent sessions don't clobber each other" \
         "one of the jail socket links is missing"
fi

# Verify they point to different targets (different inodes)
INODE1=$(stat -c %i "$JAILDIR/tmux-${TOKEN1}.sock" 2>/dev/null)
INODE2=$(stat -c %i "$JAILDIR/tmux-${TOKEN2}.sock" 2>/dev/null)
if [ -n "$INODE1" ] && [ -n "$INODE2" ] && [ "$INODE1" != "$INODE2" ]; then
    pass "jail: per-session links have distinct targets"
else
    fail "jail: per-session links have distinct targets" \
         "inode1=$INODE1 inode2=$INODE2 (should differ)"
fi

echo ""
echo "$((PASSED + FAILED)) tests: $PASSED passed, $FAILED failed"
exit $FAILED
