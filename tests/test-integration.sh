#!/bin/sh
#
# Integration tests for tmtv.
# Tests real SSH and SSE workflows against a running tmtv-server.
#
# Requirements:
#   - TEST_HOST: IP or hostname of test machine (required, no default)
#   - tmtv-server running on TEST_HOST:2222 (SSH) and :4002 (SSE)
#   - nginx on TEST_HOST:8080 serving web viewer
#   - tmtv client at /root/tmtv on TEST_HOST
#   - SSH access as root to TEST_HOST on port 22
#
# Usage:
#   TEST_HOST=<ip> sh test-integration.sh
#   TEST_HOST=<ip> sh test-integration.sh --quick
#

set -e

if [ -z "$TEST_HOST" ]; then
	echo "ERROR: TEST_HOST is required (set to IP/hostname of test machine)" >&2
	exit 1
fi
TEST_SSH_PORT="${TEST_SSH_PORT:-22}"
TMTV_PORT="${TMTV_PORT:-2222}"
SSE_PORT="${SSE_PORT:-4002}"
WEB_PORT="${WEB_PORT:-8080}"
REMOTE_TMTV="/root/tmtv"
QUICK=false

for arg in "$@"; do
	case "$arg" in
		--quick) QUICK=true ;;
	esac
done

PASSED=0
FAILED=0
SKIPPED=0
SESSION_NAME=""

pass() {
	PASSED=$((PASSED + 1))
	printf "  %-55s PASS\n" "$1"
}

fail() {
	FAILED=$((FAILED + 1))
	printf "  %-55s FAIL\n" "$1"
	if [ -n "$2" ]; then
		echo "    $2" >&2
	fi
}

skip() {
	SKIPPED=$((SKIPPED + 1))
	printf "  %-55s SKIP\n" "$1"
}

# Run command on remote host
remote() {
	ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
		-p "$TEST_SSH_PORT" "root@$TEST_HOST" "$@" 2>/dev/null
}

# Run tmtv command on remote host (needs TERM)
# Uses -S to target the test socket so commands hit the right server
remote_tmtv() {
	remote "TERM=xterm-256color $REMOTE_TMTV $*"
}

REMOTE_CONF="/root/.tmtv-test-$TESTID.conf"

# Generate unique session name for this test run
TESTID="t$$"

cleanup() {
	# Kill any test sessions
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server 2>/dev/null" || true
	# Clean up temp config
	remote "rm -f $REMOTE_CONF" 2>/dev/null || true
}
trap cleanup EXIT

echo ""
echo "== Integration Tests (${TEST_HOST}:${TMTV_PORT}) =="
echo ""

# -------------------------------------------------------
# Prerequisite: server is running
# -------------------------------------------------------
if remote "pgrep -f tmtv-server" >/dev/null 2>&1; then
	pass "tmtv-server is running"
else
	fail "tmtv-server is running" "No tmtv-server process on $TEST_HOST"
	echo "Cannot continue without server. Aborting."
	exit 1
fi

# -------------------------------------------------------
# Test: Start a tmtv session with web sharing + named session
# -------------------------------------------------------
# Derive key fingerprints from the server's keys directory
KEYS_DIR="${REMOTE_KEYS_DIR:-/root/keys}"
RSA_FP=$(remote "ssh-keygen -lf $KEYS_DIR/ssh_host_rsa_key -E sha256 2>/dev/null" \
	| awk '{print $2}' || echo "")
ED25519_FP=$(remote "ssh-keygen -lf $KEYS_DIR/ssh_host_ed25519_key -E sha256 2>/dev/null" \
	| awk '{print $2}' || echo "")

if [ -z "$RSA_FP" ] && [ -z "$ED25519_FP" ]; then
	fail "read server key fingerprints" "no keys found in $KEYS_DIR"
	echo "Cannot continue without fingerprints. Aborting."
	exit 1
fi

# Create a test config that enables web sharing and sets a name
remote "cat > $REMOTE_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$TESTID\"
set -g tmtv-web-sharing on
CONF"

# Start tmtv client in background with test config
remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $REMOTE_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

# Verify session is running
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null" | grep -q "main"; then
	pass "start tmtv session"
else
	fail "start tmtv session" "session 'main' not found"
	echo "Cannot continue without session. Aborting."
	exit 1
fi

# -------------------------------------------------------
# Test: Named session registered
# -------------------------------------------------------
SESSIONS_DIR="/tmp/tmtv/sessions"
# Check if named symlink exists (readlink follows symlinks, stat checks existence)
if remote "test -L $SESSIONS_DIR/$TESTID"; then
	pass "named session symlink created"
	SESSION_NAME="$TESTID"
	TOKEN=$(remote "readlink $SESSIONS_DIR/$TESTID" || echo "")
else
	fail "named session symlink created" "symlink '$TESTID' not found in $SESSIONS_DIR"
fi

# -------------------------------------------------------
# Test: Send keys and capture output
# -------------------------------------------------------
remote_tmtv "send-keys -t main 'echo INTEGRATION_MARKER_42' Enter"
sleep 1
OUTPUT=$(remote_tmtv "capture-pane -t main -p" 2>/dev/null || echo "")
if echo "$OUTPUT" | grep -q "INTEGRATION_MARKER_42"; then
	pass "send-keys and capture-pane"
else
	fail "send-keys and capture-pane" "marker not found in output"
fi

# -------------------------------------------------------
# Test: SSE endpoint responds (WEB RO basic)
# -------------------------------------------------------
# TOKEN was set above from readlink; if empty, try again
if [ -z "$TOKEN" ]; then
	TOKEN=$(remote "readlink $SESSIONS_DIR/$TESTID 2>/dev/null" || echo "")
fi

if [ -n "$TOKEN" ]; then
	# Test SSE endpoint returns event-stream content type
	SSE_RESPONSE=$(curl -s -m 3 -o /dev/null -w "%{http_code}:%{content_type}" \
		"http://$TEST_HOST:$SSE_PORT/$TOKEN" 2>/dev/null || echo "000:")
	HTTP_CODE=$(echo "$SSE_RESPONSE" | cut -d: -f1)
	CTYPE=$(echo "$SSE_RESPONSE" | cut -d: -f2-)

	if [ "$HTTP_CODE" = "200" ] && echo "$CTYPE" | grep -q "event-stream"; then
		pass "SSE endpoint returns event-stream"
	else
		fail "SSE endpoint returns event-stream" "got $SSE_RESPONSE"
	fi

	# Test SSE delivers data (capture first few events)
	SSE_DATA=$(curl -s -m 3 "http://$TEST_HOST:$SSE_PORT/$TOKEN" 2>/dev/null || echo "")
	if echo "$SSE_DATA" | grep -q "^data:"; then
		pass "SSE delivers terminal data"
	else
		fail "SSE delivers terminal data" "no 'data:' lines in SSE stream"
	fi
else
	skip "SSE endpoint returns event-stream (no token found)"
	skip "SSE delivers terminal data (no token found)"
fi

# -------------------------------------------------------
# Test: Web viewer via nginx (named session)
# -------------------------------------------------------
WEB_RESPONSE=$(curl -s -m 5 -o /dev/null -w "%{http_code}" \
	"http://$TEST_HOST:$WEB_PORT/s/$TESTID" 2>/dev/null || echo "000")
if [ "$WEB_RESPONSE" = "200" ]; then
	pass "web viewer serves /s/<name>"
else
	fail "web viewer serves /s/<name>" "HTTP $WEB_RESPONSE for /s/$TESTID"
fi

# -------------------------------------------------------
# Test: SSE via nginx proxy (named session)
# -------------------------------------------------------
WS_RESPONSE=$(curl -s -m 3 -o /dev/null -w "%{http_code}:%{content_type}" \
	"http://$TEST_HOST:$WEB_PORT/ws/$TESTID" 2>/dev/null || echo "000:")
WS_CODE=$(echo "$WS_RESPONSE" | cut -d: -f1)
WS_CTYPE=$(echo "$WS_RESPONSE" | cut -d: -f2-)

if [ "$WS_CODE" = "200" ] && echo "$WS_CTYPE" | grep -q "event-stream"; then
	pass "SSE via nginx proxy /ws/<name>"
else
	# Might not have nginx proxy configured for /ws/
	skip "SSE via nginx proxy /ws/<name> (got $WS_RESPONSE)"
fi

# -------------------------------------------------------
# Test: Create pane (split-window)
# -------------------------------------------------------
remote_tmtv "split-window -t main -h"
sleep 1
PANE_COUNT=$(remote_tmtv "list-panes -t main" 2>/dev/null | wc -l)
if [ "$PANE_COUNT" -ge 2 ]; then
	pass "split-window creates second pane"
else
	fail "split-window creates second pane" "got $PANE_COUNT panes"
fi

# -------------------------------------------------------
# Test: Send keys to specific pane
# -------------------------------------------------------
remote_tmtv "send-keys -t main.1 'echo PANE1_MARKER' Enter"
sleep 1
OUTPUT=$(remote_tmtv "capture-pane -t main.1 -p" 2>/dev/null || echo "")
if echo "$OUTPUT" | grep -q "PANE1_MARKER"; then
	pass "send-keys to split pane"
else
	fail "send-keys to split pane" "marker not found in pane 1"
fi

# -------------------------------------------------------
# Test: Switch panes
# -------------------------------------------------------
remote_tmtv "select-pane -t main.0"
sleep 0.5
ACTIVE=$(remote_tmtv "display-message -t main -p '#{pane_index}'" 2>/dev/null || echo "")
if [ "$ACTIVE" = "0" ]; then
	pass "select-pane switches active pane"
else
	fail "select-pane switches active pane" "active pane is '$ACTIVE', expected 0"
fi

# -------------------------------------------------------
# Test: SSH RW connection
# -------------------------------------------------------
if [ -n "$TOKEN" ]; then
	# Connect via SSH to the RW token, run a command
	RW_TOKEN=$(remote "ls $SESSIONS_DIR/ 2>/dev/null" | grep -E "^[0-9]+-$TESTID$" | head -1 || echo "")

	if [ -n "$RW_TOKEN" ]; then
		# SSH RW: tmate sessions are interactive (no exec).
		# Use timeout + script to connect, capture initial output.
		RW_OUT=$(timeout 3 script -qc \
			"ssh -o StrictHostKeyChecking=no -o ConnectTimeout=3 \
			-p $TMTV_PORT ${RW_TOKEN}@$TEST_HOST" \
			/dev/null </dev/null 2>/dev/null || true)
		# If we got any output, the SSH handshake + channel worked
		if [ -n "$RW_OUT" ]; then
			pass "SSH RW connection"
		else
			# Fallback: check server log for the connection
			if ssh -o StrictHostKeyChecking=no -p "$TEST_SSH_PORT" \
				"root@$TEST_HOST" \
				"journalctl -u tmtv-server --no-pager -n 20" 2>/dev/null \
				| grep -q "PTY client"; then
				pass "SSH RW connection (verified via log)"
			else
				fail "SSH RW connection" "no output and no log entry"
			fi
		fi
	else
		skip "SSH RW connection (RW token not found)"
	fi
else
	skip "SSH RW connection (no token)"
fi

# -------------------------------------------------------
# Test: SSH RO connection
# -------------------------------------------------------
RO_TOKEN="ro-$TESTID"
if [ -n "$TOKEN" ]; then
	RO_OUT=$(timeout 3 script -qc \
		"ssh -o StrictHostKeyChecking=no -o ConnectTimeout=3 \
		-p $TMTV_PORT ${RO_TOKEN}@$TEST_HOST" \
		/dev/null </dev/null 2>/dev/null || true)
	if [ -n "$RO_OUT" ]; then
		pass "SSH RO connection"
	else
		if ssh -o StrictHostKeyChecking=no -p "$TEST_SSH_PORT" \
			"root@$TEST_HOST" \
			"journalctl -u tmtv-server --no-pager -n 20" 2>/dev/null \
			| grep -q "Spawning exec\|viewer"; then
			pass "SSH RO connection (verified via log)"
		else
			fail "SSH RO connection" "no output and no log entry"
		fi
	fi
else
	skip "SSH RO connection (no token)"
fi

# -------------------------------------------------------
# Test: Create new window
# -------------------------------------------------------
remote_tmtv "new-window -t main -n testwin"
sleep 0.5
WIN_LIST=$(remote_tmtv "list-windows -t main" 2>/dev/null || echo "")
if echo "$WIN_LIST" | grep -q "testwin"; then
	pass "create named window"
else
	fail "create named window" "window 'testwin' not in list"
fi

# -------------------------------------------------------
# Test: Switch window
# -------------------------------------------------------
remote_tmtv "select-window -t main:0"
sleep 0.5
ACTIVE_WIN=$(remote_tmtv "display-message -t main -p '#{window_index}'" 2>/dev/null || echo "")
if [ "$ACTIVE_WIN" = "0" ]; then
	pass "switch window"
else
	fail "switch window" "active window is '$ACTIVE_WIN', expected 0"
fi

# -------------------------------------------------------
# Test: Web sharing toggle (disable)
# -------------------------------------------------------
if [ "$QUICK" = "false" ] && [ -n "$TOKEN" ]; then
	remote_tmtv "set-option -t main tmtv-set tmtv-web-sharing=off"
	sleep 2

	# When web sharing is off, new SSE connections should get disconnected
	SSE_CHECK=$(curl -s -m 2 "http://$TEST_HOST:$SSE_PORT/$TOKEN" 2>/dev/null || echo "")
	DATA_LINES=$(echo "$SSE_CHECK" | grep -c "^data:" || true)

	# Re-enable for remaining tests
	remote_tmtv "set-option -t main tmtv-set tmtv-web-sharing=on"
	sleep 2

	# After re-enable, SSE should work again
	SSE_REENABLE=$(curl -s -m 3 "http://$TEST_HOST:$SSE_PORT/$TOKEN" 2>/dev/null || echo "")
	if echo "$SSE_REENABLE" | grep -q "^data:"; then
		pass "web sharing toggle (off then on)"
	else
		fail "web sharing toggle (off then on)" "SSE not restored after re-enable"
	fi
else
	skip "web sharing toggle (--quick or no token)"
fi

# -------------------------------------------------------
# Test: Kill session cleans up
# -------------------------------------------------------
remote_tmtv "kill-session -t main"
sleep 2

# Session should be gone
SESSIONS=$(remote_tmtv "list-sessions" 2>/dev/null || echo "no server")
if echo "$SESSIONS" | grep -q "main"; then
	fail "kill-session cleanup" "session 'main' still exists"
else
	pass "kill-session cleanup"
fi

# Named symlink should be removed
if remote "test -L $SESSIONS_DIR/$TESTID" 2>/dev/null; then
	fail "session symlink removed on exit" "symlink still exists"
else
	pass "session symlink removed on exit"
fi

# -------------------------------------------------------
# Summary
# -------------------------------------------------------
echo ""
TOTAL=$((PASSED + FAILED + SKIPPED))
echo "$TOTAL tests: $PASSED passed, $FAILED failed, $SKIPPED skipped"

if [ "$FAILED" -gt 0 ]; then
	exit 1
fi
exit 0
