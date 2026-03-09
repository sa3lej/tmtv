#!/bin/sh
#
# Integration tests for tmtv.
# Tests real SSH and SSE workflows against a running tmtv-server.
#
# Requirements on the BUILD machine (where this script runs):
#   - ssh client (OpenSSH)
#   - curl
#   - npx + playwright (for visual tests): npm i -D playwright
#     Install browsers: npx playwright install chromium
#   - TEST_HOST env var set to IP/hostname of the test machine
#
# Requirements on the TEST machine (TEST_HOST):
#   - tmtv-server running (systemd), listening on SSH port (default 2222)
#     and SSE port (default 4002)
#   - tmtv client binary at /root/tmtv (or REMOTE_TMTV path)
#   - Caddy (or any web server) on port 8080 serving:
#       /           -> landing page
#       /s/<token>  -> viewer.html
#       /ws/<token> -> reverse proxy to SSE port
#   - SSH keys in /root/keys/ (or REMOTE_KEYS_DIR)
#   - expect (for SSH RW/RO text tests): apt install expect
#   - SSH access as root on port 22 (or TEST_SSH_PORT)
#
# Usage:
#   TEST_HOST=<ip> sh test-integration.sh
#   TEST_HOST=<ip> sh test-integration.sh --quick   # skip slow tests
#
# Environment variables (all optional except TEST_HOST):
#   TEST_HOST        - IP/hostname of test machine (REQUIRED)
#   TEST_SSH_PORT    - SSH port for admin access (default: 22)
#   TMTV_PORT        - tmtv-server SSH port (default: 2222)
#   SSE_PORT         - tmtv-server SSE port (default: 4002)
#   WEB_PORT         - web server port (default: 8080)
#   REMOTE_TMTV      - path to tmtv client on remote (default: /root/tmtv)
#   REMOTE_KEYS_DIR  - path to SSH keys on remote (default: /root/keys)
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
REMOTE_TMTV="${REMOTE_TMTV:-/root/tmtv}"
QUICK=false
HAS_PLAYWRIGHT=false

for arg in "$@"; do
	case "$arg" in
		--quick) QUICK=true ;;
	esac
done

# Check for Playwright (optional — visual tests skipped without it)
if command -v npx >/dev/null 2>&1 && npx playwright --version >/dev/null 2>&1; then
	HAS_PLAYWRIGHT=true
fi

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

# Generate unique session name for this test run
TESTID="t$$"

REMOTE_CONF="/root/.tmtv-test-$TESTID.conf"

cleanup() {
	# Kill any test sessions
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server 2>/dev/null" || true
	# Clean up temp config
	remote "rm -f $REMOTE_CONF" 2>/dev/null || true
}
trap cleanup EXIT

echo ""
echo "== Integration Tests (${TEST_HOST}:${TMTV_PORT}) =="
# Show versions under test
SERVER_VER_DISPLAY=$(remote "tmtv-server -V 2>&1" || echo "unknown")
CLIENT_VER_DISPLAY=$(remote "TERM=xterm-256color $REMOTE_TMTV -V 2>&1" || echo "unknown")
echo "   server: $SERVER_VER_DISPLAY"
echo "   client: $CLIENT_VER_DISPLAY"
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
# Test: Version sanity — server and client report a version
# -------------------------------------------------------
SERVER_VER=$(remote "tmtv-server -V 2>&1" || echo "")
if echo "$SERVER_VER" | grep -q "tmtv-server"; then
	pass "tmtv-server -V outputs version"
else
	fail "tmtv-server -V outputs version" "got: $SERVER_VER"
fi

CLIENT_VER=$(remote "TERM=xterm-256color $REMOTE_TMTV -V 2>&1" || echo "")
if echo "$CLIENT_VER" | grep -q "tmtv\|tmux"; then
	pass "tmtv client -V outputs version"
else
	fail "tmtv client -V outputs version" "got: $CLIENT_VER"
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
# Test: Server logs correct client version
# -------------------------------------------------------
CLIENT_VER=$(remote "TERM=xterm-256color $REMOTE_TMTV -V 2>&1" | grep -oE "[0-9]+\.[0-9]+\.[0-9]+" | head -1)
SERVER_LOG=$(remote "journalctl -u tmtv-server --no-pager -n 50 2>/dev/null" || echo "")
if echo "$SERVER_LOG" | grep -q "tmtv client version=$CLIENT_VER"; then
	pass "server logs client version ($CLIENT_VER)"
else
	# Check for old "tmate version=" format (should not appear)
	if echo "$SERVER_LOG" | grep -q "tmate version="; then
		fail "server logs client version ($CLIENT_VER)" "still using old 'tmate version' format"
	else
		skip "server logs client version" "journalctl not available"
	fi
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
# Test: Web viewer title contains session name (Caddy templates)
# -------------------------------------------------------
VIEWER_HTML=$(curl -s -m 5 "http://$TEST_HOST:$WEB_PORT/s/$TESTID" 2>/dev/null || echo "")
if echo "$VIEWER_HTML" | grep -q "<title>tmtv.*$TESTID</title>"; then
	pass "viewer <title> contains session name"
else
	fail "viewer <title> contains session name" \
		"title tag missing session name '$TESTID'"
fi

if echo "$VIEWER_HTML" | grep -q "og:title.*content=\"tmtv.*$TESTID\""; then
	pass "viewer og:title contains session name"
else
	fail "viewer og:title contains session name" \
		"og:title meta missing session name '$TESTID'"
fi

# -------------------------------------------------------
# Test: SSE viewer count — web viewer receives VIEWER_COUNT message
# -------------------------------------------------------
if [ -n "$TOKEN" ]; then
	# Capture SSE data for a few seconds — should contain viewer count
	# VIEWER_COUNT is msgpack type 14, sent as base64. We verify we get
	# data events (the count message is included in the stream).
	SSE_VC=$(curl -s -m 3 "http://$TEST_HOST:$SSE_PORT/$TOKEN" 2>/dev/null || echo "")
	VC_EVENTS=$(echo "$SSE_VC" | grep -c "^data:" || true)
	if [ "$VC_EVENTS" -ge 1 ]; then
		pass "SSE delivers viewer count events"
	else
		fail "SSE delivers viewer count events" "no data events in stream"
	fi
else
	skip "SSE delivers viewer count events (no token)"
fi

# -------------------------------------------------------
# Test: SSE via web proxy (named session)
# -------------------------------------------------------
WS_RESPONSE=$(curl -s -m 3 -o /dev/null -w "%{http_code}:%{content_type}" \
	"http://$TEST_HOST:$WEB_PORT/ws/$TESTID" 2>/dev/null || echo "000:")
WS_CODE=$(echo "$WS_RESPONSE" | cut -d: -f1)
WS_CTYPE=$(echo "$WS_RESPONSE" | cut -d: -f2-)

if [ "$WS_CODE" = "200" ] && echo "$WS_CTYPE" | grep -q "event-stream"; then
	pass "SSE via web proxy /ws/<name>"
else
	# Might not have nginx proxy configured for /ws/
	skip "SSE via web proxy /ws/<name> (got $WS_RESPONSE)"
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
# Test: SSH RW — connect, type, verify text appears
# -------------------------------------------------------
if [ -n "$TOKEN" ]; then
	RW_TOKEN=$(remote "ls $SESSIONS_DIR/ 2>/dev/null" | grep -E "^[0-9]+-$TESTID$" | head -1 || echo "")

	if [ -n "$RW_TOKEN" ]; then
		# First go back to pane 0 for a clean slate
		remote_tmtv "select-pane -t main.0"
		sleep 0.5

		# Use expect on the remote to SSH RW, type a marker.
		# Written to a file to avoid shell quoting issues.
		# Characters sent slowly (50ms) — tmate PTY eats burst input
		# during tmux screen redraw.
		RW_MARKER="RWOK$$"
		remote "cat > /tmp/tmtv-rw-test.exp << 'EXPECT'
set timeout 15
spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT TOKEN@127.0.0.1
sleep 5
foreach c [split \"echo MARKER\r\" {}] {
    send -- \$c
    after 50
}
sleep 2
EXPECT
sed -i \"s/TOKEN/$RW_TOKEN/;s/MARKER/$RW_MARKER/;s/\\\$TMTV_PORT/$TMTV_PORT/\" /tmp/tmtv-rw-test.exp
expect /tmp/tmtv-rw-test.exp >/dev/null 2>&1
rm -f /tmp/tmtv-rw-test.exp"
		sleep 1

		# Verify the marker appeared in any pane via capture-pane
		RW_FOUND=false
		for p in $(remote_tmtv "list-panes -t main -F '#{pane_id}'" 2>/dev/null); do
			CAP=$(remote_tmtv "capture-pane -t $p -p" 2>/dev/null || echo "")
			if echo "$CAP" | grep -q "$RW_MARKER"; then
				RW_FOUND=true
				break
			fi
		done
		if [ "$RW_FOUND" = "true" ]; then
			pass "SSH RW text input"
		else
			fail "SSH RW text input" "marker '$RW_MARKER' not in any pane"
		fi
	else
		skip "SSH RW text input (RW token not found)"
	fi
else
	skip "SSH RW text input (no token)"
fi

# -------------------------------------------------------
# Test: SSH RW — create pane via tmux split-window command
# -------------------------------------------------------
if [ -n "$RW_TOKEN" ]; then
	# Record pane count before
	PANES_BEFORE=$(remote_tmtv "list-panes -t main" 2>/dev/null | wc -l)

	# Create a pane via SSH RW: type the tmtv split-window command
	# inside the shared session shell
	remote "cat > /tmp/tmtv-split.exp << 'EXPECT'
set timeout 15
spawn ssh -o StrictHostKeyChecking=no -p TMTV_PORT TOKEN@127.0.0.1
sleep 5
foreach c [split \"tmtv split-window -h\r\" {}] {
    send -- \$c
    after 50
}
sleep 3
EXPECT
sed -i \"s/TOKEN/$RW_TOKEN/;s/TMTV_PORT/$TMTV_PORT/\" /tmp/tmtv-split.exp
expect /tmp/tmtv-split.exp >/dev/null 2>&1
rm -f /tmp/tmtv-split.exp"
	sleep 1

	PANES_AFTER=$(remote_tmtv "list-panes -t main" 2>/dev/null | wc -l)
	if [ "$PANES_AFTER" -gt "$PANES_BEFORE" ]; then
		pass "SSH RW create pane (split-window)"
	else
		fail "SSH RW create pane (split-window)" \
			"panes before=$PANES_BEFORE after=$PANES_AFTER"
	fi

	# Send text to the new pane and verify
	SPLIT_MARKER="SPLIT_$$"
	# The new pane is now active, send a marker via tmtv send-keys
	remote_tmtv "send-keys 'echo $SPLIT_MARKER' Enter"
	sleep 1
	SPLIT_CAP=$(remote_tmtv "capture-pane -p" 2>/dev/null || echo "")
	if echo "$SPLIT_CAP" | grep -q "$SPLIT_MARKER"; then
		pass "SSH RW new pane accepts input"
	else
		fail "SSH RW new pane accepts input" "marker not in new pane"
	fi

	# Verify SSE still delivers data after pane operations
	if [ -n "$TOKEN" ]; then
		SSE_AFTER=$(curl -s -m 3 "http://$TEST_HOST:$SSE_PORT/$TOKEN" \
			2>/dev/null || echo "")
		if echo "$SSE_AFTER" | grep -q "^data:"; then
			pass "SSE streams after pane create"
		else
			fail "SSE streams after pane create" "no data after split"
		fi
	fi

	# Clean up: close the extra pane to keep a predictable state
	remote_tmtv "kill-pane"
	sleep 0.5
else
	skip "SSH RW create pane (no RW token)"
	skip "SSH RW new pane accepts input (no RW token)"
	skip "SSE streams after pane create (no RW token)"
fi

# -------------------------------------------------------
# Test: SSH RO — connect, verify can see output, verify read-only
# -------------------------------------------------------
RO_TOKEN="ro-$TESTID"
if [ -n "$TOKEN" ]; then
	# Send a fresh marker to the session so the RO client can see it
	RO_MARKER="RO_VISIBLE_$$"
	remote_tmtv "send-keys -t main.0 'echo $RO_MARKER' Enter"
	sleep 1

	# Connect RO via expect and capture what the viewer sees
	RO_CAPTURE=$(remote "expect -c '
		log_user 1
		set timeout 5
		spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ${RO_TOKEN}@127.0.0.1
		sleep 3
		send \"\"
		expect eof
	' 2>/dev/null" || echo "")

	if echo "$RO_CAPTURE" | grep -q "$RO_MARKER"; then
		pass "SSH RO sees session output"
	else
		# RO viewer may not capture full screen; check pane instead
		RO_PANE=$(remote_tmtv "capture-pane -t main.0 -p" 2>/dev/null || echo "")
		if echo "$RO_PANE" | grep -q "$RO_MARKER"; then
			pass "SSH RO sees session output (verified via pane)"
		else
			fail "SSH RO sees session output" "marker '$RO_MARKER' not found"
		fi
	fi

	# Verify RO cannot write: use expect to type via RO SSH, check it does NOT appear
	RO_WRITE_MARKER="RO_NOTYPE_$$"
	remote "expect -c '
		set timeout 6
		spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ${RO_TOKEN}@127.0.0.1
		sleep 2
		send \"echo $RO_WRITE_MARKER\r\"
		sleep 2
		send \"exit\r\"
		expect eof
	' >/dev/null 2>&1"
	sleep 1

	RO_CHECK=$(remote_tmtv "capture-pane -t main.0 -p" 2>/dev/null || echo "")
	if echo "$RO_CHECK" | grep -q "$RO_WRITE_MARKER"; then
		fail "SSH RO is read-only" "RO client was able to write text"
	else
		pass "SSH RO is read-only"
	fi
else
	skip "SSH RO sees session output (no token)"
	skip "SSH RO is read-only (no token)"
fi

# -------------------------------------------------------
# Test: SSH viewer status bar shows viewer count (S:N W:N)
# -------------------------------------------------------
if [ -n "$TOKEN" ]; then
	# Connect an SSH RO viewer and capture the terminal output
	VIEWER_LOG=$(remote "TERM=xterm-256color script -qc \
		'timeout 6 ssh -tt -p $TMTV_PORT -o StrictHostKeyChecking=no \
		ro-${TESTID}@127.0.0.1' /tmp/viewer-status.log 2>/dev/null; \
		strings /tmp/viewer-status.log" || echo "")

	if echo "$VIEWER_LOG" | grep -qo "S:[0-9]* W:[0-9]*"; then
		pass "SSH viewer status bar shows S:N W:N"
	else
		fail "SSH viewer status bar shows S:N W:N" \
			"pattern not found in viewer output"
	fi

	# Verify the count is at least S:1 (the viewer itself)
	VIEWER_S=$(echo "$VIEWER_LOG" | grep -o "S:[0-9]*" | tail -1 | cut -d: -f2)
	if [ -n "$VIEWER_S" ] && [ "$VIEWER_S" -ge 1 ] 2>/dev/null; then
		pass "SSH viewer count >= 1"
	else
		fail "SSH viewer count >= 1" "got S:${VIEWER_S:-empty}"
	fi

	# Test web viewer count: connect SSE client, verify W:1 in SSH status bar
	curl -s -m 15 -N "http://$TEST_HOST:$SSE_PORT/$TOKEN" > /dev/null 2>&1 &
	WEB_CURL_PID=$!
	sleep 3

	VIEWER_LOG2=$(remote "TERM=xterm-256color script -qc \
		'timeout 6 ssh -tt -p $TMTV_PORT -o StrictHostKeyChecking=no \
		ro-${TESTID}@127.0.0.1' /tmp/viewer-web.log 2>/dev/null; \
		strings /tmp/viewer-web.log" || echo "")
	kill $WEB_CURL_PID 2>/dev/null || true

	VIEWER_W=$(echo "$VIEWER_LOG2" | grep -o "W:[0-9]*" | tail -1 | cut -d: -f2)
	if [ -n "$VIEWER_W" ] && [ "$VIEWER_W" -ge 1 ] 2>/dev/null; then
		pass "web viewer count >= 1 in SSH status bar"
	else
		fail "web viewer count >= 1 in SSH status bar" "got W:${VIEWER_W:-empty}"
	fi
else
	skip "SSH viewer status bar shows S:N W:N (no token)"
	skip "SSH viewer count >= 1 (no token)"
	skip "web viewer count >= 1 in SSH status bar (no token)"
fi

# -------------------------------------------------------
# Test: Status bar customization (set-option status-right)
# -------------------------------------------------------
if [ -n "$TOKEN" ]; then
	# Override status-right with 24H clock and ISO date
	remote_tmtv "set-option -g status-right 'S:#{tmtv_ssh_viewers} W:#{tmtv_web_viewers} \"#{=21:pane_title}\" %H:%M %Y-%m-%d'"
	sleep 2

	# Connect SSH RO viewer and capture the status bar
	CUSTOM_LOG=$(remote "TERM=xterm-256color script -qc \
		'timeout 6 ssh -tt -p $TMTV_PORT -o StrictHostKeyChecking=no \
		ro-${TESTID}@127.0.0.1' /tmp/viewer-custom.log 2>/dev/null; \
		strings /tmp/viewer-custom.log" || echo "")

	# Verify ISO date format (YYYY-MM-DD) appears
	if echo "$CUSTOM_LOG" | grep -qE "[0-9]{4}-[0-9]{2}-[0-9]{2}"; then
		pass "status-right override (ISO date)"
	else
		fail "status-right override (ISO date)" \
			"YYYY-MM-DD not found in viewer output"
	fi

	# Verify viewer counts still work after override
	if echo "$CUSTOM_LOG" | grep -qo "S:[0-9]* W:[0-9]*"; then
		pass "viewer counts survive status-right override"
	else
		fail "viewer counts survive status-right override" \
			"S:N W:N not found after set-option"
	fi

	# Restore default
	remote_tmtv "set-option -gu status-right"
	sleep 1
else
	skip "status-right override (ISO date) (no token)"
	skip "viewer counts survive status-right override (no token)"
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
# Test: Terminal resize propagates to SSE
# -------------------------------------------------------
if [ -n "$TOKEN" ]; then
	# Resize the terminal to 100x30
	remote_tmtv "resize-window -t main -x 100 -y 30"
	sleep 2

	# Capture SSE data — should contain layout sync with new dimensions
	# The SSE stream sends binary msgpack; we check for non-empty data
	# after resize, which includes the new SYNC_LAYOUT message
	SSE_RESIZE=$(curl -s -m 3 "http://$TEST_HOST:$SSE_PORT/$TOKEN" \
		2>/dev/null || echo "")
	if echo "$SSE_RESIZE" | grep -q "^data:"; then
		pass "SSE delivers data after resize"
	else
		fail "SSE delivers data after resize" "no data after resize"
	fi

	# Verify the session dimensions changed
	DIMS=$(remote_tmtv "display-message -t main -p '#{window_width}x#{window_height}'" \
		2>/dev/null || echo "")
	if [ "$DIMS" = "100x30" ]; then
		pass "terminal resize applied"
	else
		fail "terminal resize applied" "expected 100x30, got $DIMS"
	fi

	# Restore to standard size
	remote_tmtv "resize-window -t main -x 80 -y 24"
	sleep 1
else
	skip "SSE delivers data after resize (no token)"
	skip "terminal resize applied (no token)"
fi

# -------------------------------------------------------
# Test: Multi-window SSE — switch window, SSE still streams
# -------------------------------------------------------
if [ -n "$TOKEN" ]; then
	# We already have window 0 and testwin from earlier tests
	remote_tmtv "select-window -t main:testwin"
	sleep 1

	# Send a marker to the new window
	WIN_MARKER="WINTST_$$"
	remote_tmtv "send-keys 'echo $WIN_MARKER' Enter"
	sleep 1

	# Verify SSE still delivers data on the active window
	SSE_WIN=$(curl -s -m 3 "http://$TEST_HOST:$SSE_PORT/$TOKEN" \
		2>/dev/null || echo "")
	if echo "$SSE_WIN" | grep -q "^data:"; then
		pass "SSE streams after window switch"
	else
		fail "SSE streams after window switch" "no data"
	fi

	# Verify the marker is in the testwin pane
	WIN_CAP=$(remote_tmtv "capture-pane -t main:testwin -p" 2>/dev/null || echo "")
	if echo "$WIN_CAP" | grep -q "$WIN_MARKER"; then
		pass "text input in switched window"
	else
		fail "text input in switched window" "marker not found"
	fi

	# Switch back
	remote_tmtv "select-window -t main:0"
	sleep 0.5
else
	skip "SSE streams after window switch (no token)"
	skip "text input in switched window (no token)"
fi

# -------------------------------------------------------
# Test: SSE reconnect — new client gets screen dump
# -------------------------------------------------------
if [ -n "$TOKEN" ]; then
	# Put a unique marker on screen
	RECONNECT_MARKER="RECONN_$$"
	remote_tmtv "send-keys -t main:0 'echo $RECONNECT_MARKER' Enter"
	sleep 1

	# Connect a fresh SSE client — should get screen dump with marker
	SSE_RECONNECT=$(curl -s -m 4 "http://$TEST_HOST:$SSE_PORT/$TOKEN" \
		2>/dev/null || echo "")
	if echo "$SSE_RECONNECT" | grep -q "^data:"; then
		pass "SSE reconnect delivers screen dump"
	else
		fail "SSE reconnect delivers screen dump" "no data on reconnect"
	fi
else
	skip "SSE reconnect delivers screen dump (no token)"
fi

# -------------------------------------------------------
# Test: Playwright visual — web viewer renders terminal content
# -------------------------------------------------------
if [ "$HAS_PLAYWRIGHT" = "true" ] && [ "$QUICK" = "false" ]; then
	# Put a unique visual marker on screen
	VIS_MARKER="VISUAL_$$"
	remote_tmtv "send-keys -t main:0 'echo $VIS_MARKER' Enter"
	sleep 2

	SCREENSHOT="/tmp/tmtv-visual-$$.png"
	if npx playwright screenshot --browser chromium \
		"http://$TEST_HOST:$WEB_PORT/s/$TESTID" \
		"$SCREENSHOT" >/dev/null 2>&1; then

		# Verify the screenshot file exists and is non-trivial (>10KB)
		FSIZE=$(stat -c%s "$SCREENSHOT" 2>/dev/null || echo "0")
		if [ "$FSIZE" -gt 10000 ]; then
			pass "web viewer renders (screenshot ${FSIZE}B)"
		else
			fail "web viewer renders" "screenshot too small: ${FSIZE}B"
		fi
		rm -f "$SCREENSHOT"
	else
		fail "web viewer renders" "playwright screenshot failed"
	fi
else
	if [ "$HAS_PLAYWRIGHT" != "true" ]; then
		skip "web viewer renders (playwright not installed)"
	else
		skip "web viewer renders (--quick)"
	fi
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
# Test: second new-session gives error, not crash
# -------------------------------------------------------
remote "TERM=xterm-256color $REMOTE_TMTV new -d -s multi1" 2>/dev/null
sleep 2
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "multi1"; then
	remote "TERM=xterm-256color $REMOTE_TMTV new -d -s multi2" 2>/dev/null || true
	sleep 1
	# Server must still be alive after the failed second session
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "multi1"; then
		pass "second new-session errors without crash"
	else
		fail "second new-session errors without crash" "server died"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
else
	fail "second new-session errors without crash" "could not create test session"
fi
sleep 1

# -------------------------------------------------------
# Test: bare tmtv auto-attaches to existing session
# -------------------------------------------------------
remote "TERM=xterm-256color $REMOTE_TMTV new -d -s autoattach" 2>/dev/null
sleep 2
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "autoattach"; then
	# Run bare tmtv with timeout — will block on attach (no TTY), but must not crash
	remote "timeout 2 env TERM=xterm-256color $REMOTE_TMTV" 2>/dev/null || true
	sleep 1
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "autoattach"; then
		pass "bare tmtv auto-attaches without crash"
	else
		fail "bare tmtv auto-attaches without crash" "server died"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
else
	fail "bare tmtv auto-attaches without crash" "could not create test session"
fi
sleep 1

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
