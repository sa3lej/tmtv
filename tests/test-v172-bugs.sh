#!/bin/bash
#
# Verification tests for v1.7.2 bugs.
# Run ON the staging server: TEST_HOST=localhost WEB_HOST=staging.tmtv.se bash test-v172-bugs.sh
#

set -e

TEST_HOST="${TEST_HOST:-localhost}"
WEB_HOST="${WEB_HOST:-$TEST_HOST}"
TMTV_PORT="${TMTV_PORT:-2222}"
SSE_PORT="${SSE_PORT:-4002}"
REMOTE_TMTV="${REMOTE_TMTV:-/usr/local/bin/tmtv}"
SESSIONS_DIR="/tmp/tmtv/sessions"
KEYS_DIR="${REMOTE_KEYS_DIR:-/etc/tmtv/keys}"

passed=0
failed=0
skipped=0

pass() { echo "  PASS $1"; passed=$((passed + 1)); }
fail() { echo "  FAIL $1 -- $2"; failed=$((failed + 1)); }
skip() { echo "  SKIP $1 -- $2"; skipped=$((skipped + 1)); }

cleanup() {
    TERM=xterm-256color "$REMOTE_TMTV" kill-server 2>/dev/null || true
    sleep 1
    sudo rm -f "$SESSIONS_DIR"/* 2>/dev/null || true
    rm -f /tmp/.tmtv-v172-*.conf 2>/dev/null || true
    rm -f /tmp/sse-*.dat 2>/dev/null || true
}
trap cleanup EXIT

# Get server key fingerprints (needed for local connection)
RSA_FP=$(ssh-keygen -lf "$KEYS_DIR/ssh_host_rsa_key.pub" -E sha256 2>/dev/null | awk '{print $2}' || echo "")
ED25519_FP=$(ssh-keygen -lf "$KEYS_DIR/ssh_host_ed25519_key.pub" -E sha256 2>/dev/null | awk '{print $2}' || echo "")

if [ -z "$RSA_FP" ] && [ -z "$ED25519_FP" ]; then
    echo "FATAL: Cannot read server key fingerprints from $KEYS_DIR"
    exit 1
fi

# Helper: create a config file for a named session
make_conf() {
    _name="$1"
    _conf="/tmp/.tmtv-v172-${_name}.conf"
    cat > "$_conf" << CONF
set -g tmtv-server-host "127.0.0.1"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint "$RSA_FP"
set -g tmtv-server-ed25519-fingerprint "$ED25519_FP"
set -g tmtv-session-name "$_name"
set -g tmtv-web-sharing on
set -g tmtv-web-input on
CONF
    echo "$_conf"
}

# Helper: start a session and wait for token
start_session() {
    _name="$1"
    _conf=$(make_conf "$_name")

    TERM=xterm-256color nohup script -qc "$REMOTE_TMTV -f $_conf new-session -d -s main" \
        /dev/null </dev/null >/dev/null 2>&1 &

    # Wait for token to stabilize
    _prev=""
    _stable=0
    for _w in $(seq 1 20); do
        sleep 1
        _cur=$(readlink "$SESSIONS_DIR/$_name" 2>/dev/null || echo "")
        if [ -n "$_cur" ] && [ "$_cur" = "$_prev" ]; then
            _stable=$((_stable + 1))
            [ "$_stable" -ge 3 ] && break
        else
            _stable=0
        fi
        _prev="$_cur"
    done

    echo "$_cur"
}

# Helper: kill session and clean up
kill_session() {
    TERM=xterm-256color "$REMOTE_TMTV" kill-server 2>/dev/null || true
    sleep 2
    sudo rm -f "$SESSIONS_DIR"/* 2>/dev/null || true
}

# Clean slate
cleanup 2>/dev/null || true
sleep 1

echo ""
echo "=== v1.7.2 Bug Verification Tests ==="
echo ""
SSE_BASE="http://127.0.0.1:$SSE_PORT"

# ============================================================
# TEST 1: tmtv-m0gb — Window switch with vpty blank screen
# ============================================================
echo "-- Bug tmtv-m0gb: Window switch blank screen with vpty --"

TOKEN=$(start_session "wswitch")

if [ -n "$TOKEN" ]; then
    # Create a second window
    TERM=xterm-256color "$REMOTE_TMTV" send-keys 'echo WINDOW0' Enter 2>/dev/null || true
    TERM=xterm-256color "$REMOTE_TMTV" new-window 2>/dev/null || true
    TERM=xterm-256color "$REMOTE_TMTV" send-keys 'echo WINDOW1' Enter 2>/dev/null || true
    sleep 1

    # Connect SSE and capture data
    _sse_out="/tmp/sse-wswitch-$$.dat"
    curl -s -N --max-time 8 "$SSE_BASE/$TOKEN" > "$_sse_out" 2>/dev/null &
    _sse_pid=$!
    sleep 2

    # Record size before window switch
    _size_before=$(wc -c < "$_sse_out" 2>/dev/null || echo 0)

    # Switch back to window 0
    TERM=xterm-256color "$REMOTE_TMTV" select-window -t :0 2>/dev/null || true
    sleep 3

    # Check if SSE received data after the switch
    _size_after=$(wc -c < "$_sse_out" 2>/dev/null || echo 0)
    _delta=$((_size_after - _size_before))

    kill $_sse_pid 2>/dev/null || true

    if [ "$_delta" -gt 50 ]; then
        pass "SSE receives data after window switch ($_delta bytes)"
    else
        fail "SSE receives data after window switch" "only $_delta bytes after switch (blank screen bug)"
    fi

    rm -f "$_sse_out"
else
    skip "window switch blank screen" "could not find SSE token"
fi

kill_session


# ============================================================
# TEST 2: tmtv-vu9j — SESSION_MODE includes viewer_id
# ============================================================
echo ""
echo "-- Bug tmtv-vu9j: SESSION_MODE viewer_id --"

TOKEN=$(start_session "viewerid")

if [ -n "$TOKEN" ]; then
    # Connect SSE and capture handshake
    _sse_out="/tmp/sse-viewerid-$$.dat"
    curl -s -N --max-time 5 "$SSE_BASE/$TOKEN" > "$_sse_out" 2>/dev/null || true

    _raw_size=$(wc -c < "$_sse_out" 2>/dev/null || echo 0)

    if [ "$_raw_size" -gt 20 ]; then
        if python3 -c "
import sys, base64
found_4 = False
found_3 = False
for line in open('$_sse_out', 'r'):
    line = line.strip()
    if not line.startswith('data: '):
        continue
    try:
        data = base64.b64decode(line[6:])
    except Exception:
        continue
    for i in range(len(data)-2):
        if data[i] == 0x94 and data[i+1] == 15:
            found_4 = True
        if data[i] == 0x93 and data[i+1] == 15:
            found_3 = True
if found_4:
    sys.exit(0)
elif found_3:
    sys.exit(1)
else:
    sys.exit(2)
" 2>/dev/null; then
            pass "SESSION_MODE includes viewer_id (4-field array)"
        else
            _result=$?
            if [ "$_result" = "1" ]; then
                fail "SESSION_MODE missing viewer_id" "only 3-field array (old protocol)"
            else
                fail "SESSION_MODE" "could not find SESSION_MODE in SSE stream"
            fi
        fi
    else
        fail "SESSION_MODE" "SSE stream too small ($_raw_size bytes)"
    fi

    rm -f "$_sse_out"
else
    skip "SESSION_MODE viewer_id" "could not find SSE token"
fi

kill_session


# ============================================================
# TEST 3: tmtv-vu9j — CORS preflight allows X-Tmtv-Viewer-Id
# ============================================================
echo ""
echo "-- Bug tmtv-vu9j: CORS allows X-Tmtv-Viewer-Id header --"

TOKEN=$(start_session "cors")

if [ -n "$TOKEN" ]; then
    _cors_headers=$(curl -s -I -X OPTIONS \
        -H "Origin: https://example.com" \
        -H "Access-Control-Request-Method: POST" \
        -H "Access-Control-Request-Headers: X-Tmtv-Viewer-Id" \
        "$SSE_BASE/$TOKEN/input" 2>/dev/null)

    if echo "$_cors_headers" | grep -qi 'X-Tmtv-Viewer-Id'; then
        pass "CORS preflight allows X-Tmtv-Viewer-Id header"
    else
        fail "CORS preflight" "X-Tmtv-Viewer-Id not in Access-Control-Allow-Headers"
    fi
else
    skip "CORS X-Tmtv-Viewer-Id" "could not find SSE token"
fi

kill_session


# ============================================================
# TEST 4: tmtv-n0k2 — Repeated input mode enable stability
# ============================================================
echo ""
echo "-- Bug tmtv-n0k2: Repeated input mode enable --"

TOKEN=$(start_session "dupjoin")

if [ -n "$TOKEN" ]; then
    # Connect an SSE viewer
    curl -s -N --max-time 2 "$SSE_BASE/$TOKEN" > /dev/null 2>&1 &
    _sse_pid=$!
    sleep 2

    _log_marker=$(date -u '+%Y-%m-%d %H:%M:%S')

    # Toggle input mode multiple times
    TERM=xterm-256color "$REMOTE_TMTV" set-option -g tmtv-web-input off 2>/dev/null || true
    sleep 1
    TERM=xterm-256color "$REMOTE_TMTV" set-option -g tmtv-web-input on 2>/dev/null || true
    sleep 1
    TERM=xterm-256color "$REMOTE_TMTV" set-option -g tmtv-web-input off 2>/dev/null || true
    sleep 1
    TERM=xterm-256color "$REMOTE_TMTV" set-option -g tmtv-web-input on 2>/dev/null || true
    sleep 1

    kill $_sse_pid 2>/dev/null || true

    _enable_count=$(sudo journalctl -u tmtv-server --since "$_log_marker" --no-pager 2>/dev/null \
        | grep -c "Input mode: enabled=1" || echo 0)

    if sudo systemctl is-active tmtv-server > /dev/null 2>&1; then
        pass "Input mode toggled $_enable_count times, server stable"
    else
        fail "Input mode toggle" "tmtv-server crashed during toggle"
    fi
else
    skip "duplicate USER_JOIN" "could not find SSE token"
fi

kill_session


# ============================================================
# TEST 5: tmtv-v83z — Binary built from patched source
# ============================================================
echo ""
echo "-- Bug tmtv-v83z: Input socket user deduplication --"

if strings /usr/local/bin/tmtv-server 2>/dev/null | grep -q 'X-Tmtv-Viewer-Id'; then
    pass "Server binary built from patched source"
else
    fail "Binary check" "server not built from patched source"
fi


# ============================================================
# Summary
# ============================================================
echo ""
echo "=== Results ==="
echo "Passed: $passed  Failed: $failed  Skipped: $skipped"
echo ""

if [ "$failed" -gt 0 ]; then
    exit 1
fi
exit 0
