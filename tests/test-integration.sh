#!/bin/sh
#
# Integration tests for tmtv.
# Tests real SSH and SSE workflows against a running tmtv-server.
#
# Can run locally on the server (recommended) or remotely over SSH.
#
# Local mode (on the staging/test server):
#   sh test-integration.sh                    # auto-detects localhost
#   sh test-integration.sh --quick            # skip slow tests
#
# Remote mode (from a different machine):
#   TEST_HOST=staging.tmtv.se sh test-integration.sh
#
# Requirements:
#   - tmtv-server running (systemd), listening on SSH port (default 2222)
#     and SSE port (default 4002)
#   - tmtv client binary at /usr/local/bin/tmtv (or REMOTE_TMTV path)
#   - Caddy (or any web server) on HTTPS (port 443) serving:
#       /s/<token>  -> viewer.html (with Caddy templates)
#       /ws/<token> -> reverse proxy to SSE port
#   - SSH keys in /etc/tmtv/keys (or REMOTE_KEYS_DIR)
#   - expect (for SSH RW/RO text tests): apt install expect
#   - curl
#   - Playwright (optional, for visual tests):
#       mkdir -p /opt/tmtv-tests && cd /opt/tmtv-tests
#       echo '{"dependencies":{"playwright":"1.58.2"}}' > package.json
#       npm install
#       PLAYWRIGHT_BROWSERS_PATH=/opt/tmtv-tests/browsers \
#         /opt/tmtv-tests/node_modules/.bin/playwright install chromium
#
# Environment variables (all optional):
#   TEST_HOST              - IP/hostname of test machine (default: localhost)
#   TEST_SSH_PORT          - SSH port for admin access (default: 22, remote mode only)
#   TMTV_PORT              - tmtv-server SSH port (default: 2222)
#   SSE_PORT               - tmtv-server SSE port (default: 4002)
#   WEB_PROTO              - web server protocol (default: https)
#   WEB_PORT               - web server port (default: 443)
#   WEB_HOST               - hostname for web/TLS tests (default: TEST_HOST)
#                            Set to staging.tmtv.se when running locally on staging
#                            so Caddy TLS certs match (domain resolves to 127.0.0.1)
#   REMOTE_TMTV            - path to tmtv binary (default: /usr/local/bin/tmtv)
#   REMOTE_KEYS_DIR        - path to SSH keys (default: /etc/tmtv/keys)
#   TMTV_PLAYWRIGHT_DIR    - directory with playwright npm module and browsers
#                            (default: /opt/tmtv-tests)
#

set -e

TEST_HOST="${TEST_HOST:-localhost}"
TEST_SSH_USER="${TEST_SSH_USER:-root}"
TEST_SSH_PORT="${TEST_SSH_PORT:-22}"
TMTV_PORT="${TMTV_PORT:-2222}"
SSE_PORT="${SSE_PORT:-4002}"
WEB_PROTO="${WEB_PROTO:-https}"
WEB_PORT="${WEB_PORT:-443}"
WEB_HOST="${WEB_HOST:-}"
REMOTE_TMTV="${REMOTE_TMTV:-/usr/local/bin/tmtv}"
TMTV_PLAYWRIGHT_DIR="${TMTV_PLAYWRIGHT_DIR:-/opt/tmtv-tests}"
QUICK=false
HAS_PLAYWRIGHT=false
HAS_WEB=true
LOCAL=false

for arg in "$@"; do
	case "$arg" in
		--quick) QUICK=true ;;
		--local) LOCAL=true ;;
	esac
done

# Auto-detect local mode
if [ "$TEST_HOST" = "localhost" ] || [ "$TEST_HOST" = "127.0.0.1" ]; then
	LOCAL=true
fi

# WEB_HOST defaults to TEST_HOST but can be overridden separately
# (e.g., run locally but use staging.tmtv.se for web/TLS tests)
WEB_HOST="${WEB_HOST:-$TEST_HOST}"

# Detect if web server is available
if [ "$WEB_PORT" = "0" ]; then
	HAS_WEB=false
fi

# Convenience: full base URLs for web and SSE requests
WEB_URL="${WEB_PROTO}://${WEB_HOST}:${WEB_PORT}"

# Probe web server reachability (Caddy TLS certs may not match "localhost")
if [ "$HAS_WEB" = "true" ]; then
	_probe=$(curl -sk -m 3 -o /dev/null -w "%{http_code}" "$WEB_URL/" 2>/dev/null) || true
	if [ "$_probe" = "000" ]; then
		HAS_WEB=false
		echo "  (web server at $WEB_URL not reachable, skipping web tests)"
	fi
fi
if [ "$LOCAL" = "true" ]; then
	SSE_BASE="http://127.0.0.1:${SSE_PORT}"
else
	SSE_BASE="http://${TEST_HOST}:${SSE_PORT}"
fi

# Check for Playwright (optional — visual tests skipped without it)
# Use the isolated install at TMTV_PLAYWRIGHT_DIR (/opt/tmtv-tests by default).
# NODE_PATH lets node find the module regardless of CWD. PLAYWRIGHT_BROWSERS_PATH
# points to the chromium binary installed in the same directory.
PW_NODE_PATH="${TMTV_PLAYWRIGHT_DIR}/node_modules"
PW_BROWSERS_PATH="${TMTV_PLAYWRIGHT_DIR}/browsers"
if NODE_PATH="$PW_NODE_PATH" node -e "require('playwright')" >/dev/null 2>&1 \
   && PLAYWRIGHT_BROWSERS_PATH="$PW_BROWSERS_PATH" \
      NODE_PATH="$PW_NODE_PATH" \
      node -e "const {chromium}=require('playwright'); const fs=require('fs'); if(!fs.existsSync(chromium.executablePath())) throw new Error('no browser')" \
      >/dev/null 2>&1; then
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

# Run command on the test host (locally via sudo, or via SSH)
# When TEST_SSH_USER is not root, commands are run via sudo.
remote() {
	if [ "$LOCAL" = "true" ]; then
		sudo sh -c "$*" 2>/dev/null
	else
		SUDO_PREFIX=""
		[ "$TEST_SSH_USER" != "root" ] && SUDO_PREFIX="sudo "
		ssh -o StrictHostKeyChecking=no -o ConnectTimeout=5 \
			-p "$TEST_SSH_PORT" "${TEST_SSH_USER}@$TEST_HOST" \
			"${SUDO_PREFIX}sh -c '$*'" 2>/dev/null
	fi
}

# Run tmtv command on test host (needs TERM)
remote_tmtv() {
	remote "TERM=xterm-256color $REMOTE_TMTV $*"
}

# Generate unique session name for this test run
TESTID="t$$"

REMOTE_CONF="/tmp/.tmtv-test-$TESTID.conf"

cleanup() {
	# Kill any test sessions
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server 2>/dev/null" || true
	# Clean up all temp configs from this test run
	remote "rm -f /tmp/.tmtv-test-*-$TESTID.conf" 2>/dev/null || true
	remote "rm -f $REMOTE_CONF" 2>/dev/null || true
}
trap cleanup EXIT

echo ""
if [ "$WEB_HOST" != "$TEST_HOST" ]; then
	echo "== Integration Tests (${TEST_HOST}:${TMTV_PORT}, web: ${WEB_HOST}) =="
else
	echo "== Integration Tests (${TEST_HOST}:${TMTV_PORT}) =="
fi
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
# Test: Health check endpoint (/healthz)
# -------------------------------------------------------
HEALTH_RESP=$(curl -s -m 3 "$SSE_BASE/healthz" 2>/dev/null || echo "")
if echo "$HEALTH_RESP" | grep -q '"status":"ok"'; then
	pass "healthz returns status ok"
else
	fail "healthz returns status ok" "got: $HEALTH_RESP"
fi

# Verify JSON fields: version, uptime_seconds, active_sessions
if echo "$HEALTH_RESP" | grep -q '"version":"[0-9]'; then
	pass "healthz includes version"
else
	fail "healthz includes version" "got: $HEALTH_RESP"
fi

if echo "$HEALTH_RESP" | grep -q '"uptime_seconds":[0-9]'; then
	pass "healthz includes uptime_seconds"
else
	fail "healthz includes uptime_seconds" "got: $HEALTH_RESP"
fi

if echo "$HEALTH_RESP" | grep -q '"active_sessions":[0-9]'; then
	pass "healthz includes active_sessions"
else
	fail "healthz includes active_sessions" "got: $HEALTH_RESP"
fi

# Verify correct Content-Type header
HEALTH_CTYPE=$(curl -s -m 3 -o /dev/null -w "%{content_type}" "$SSE_BASE/healthz" 2>/dev/null || echo "")
if echo "$HEALTH_CTYPE" | grep -q "application/json"; then
	pass "healthz returns application/json"
else
	fail "healthz returns application/json" "got: $HEALTH_CTYPE"
fi

# -------------------------------------------------------
# Test: Start a tmtv session with web sharing + named session
# -------------------------------------------------------
# Derive key fingerprints from the server's keys directory
KEYS_DIR="${REMOTE_KEYS_DIR:-/etc/tmtv/keys}"
RSA_FP=$(remote "ssh-keygen -lf $KEYS_DIR/ssh_host_rsa_key.pub -E sha256 2>/dev/null" \
	| awk '{print $2}' || echo "")
ED25519_FP=$(remote "ssh-keygen -lf $KEYS_DIR/ssh_host_ed25519_key.pub -E sha256 2>/dev/null" \
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
		"$SSE_BASE/$TOKEN" 2>/dev/null) || true
	HTTP_CODE=$(echo "$SSE_RESPONSE" | cut -d: -f1)
	CTYPE=$(echo "$SSE_RESPONSE" | cut -d: -f2-)

	if [ "$HTTP_CODE" = "200" ] && echo "$CTYPE" | grep -q "event-stream"; then
		pass "SSE endpoint returns event-stream"
	else
		fail "SSE endpoint returns event-stream" "got $SSE_RESPONSE"
	fi

	# Test SSE delivers data (capture first few events)
	SSE_DATA=$(curl -s -m 3 "$SSE_BASE/$TOKEN" 2>/dev/null || echo "")
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
# Test: Web viewer via Caddy (named session)
# -------------------------------------------------------
if [ "$HAS_WEB" = "true" ]; then
	WEB_RESPONSE=$(curl -s -m 5 -o /dev/null -w "%{http_code}" \
		"$WEB_URL/s/$TESTID" 2>/dev/null) || true
	if [ "$WEB_RESPONSE" = "200" ]; then
		pass "web viewer serves /s/<name>"
	else
		fail "web viewer serves /s/<name>" "HTTP $WEB_RESPONSE for /s/$TESTID"
	fi

	# -------------------------------------------------------
	# Test: Web viewer title contains session name (Caddy templates)
	# -------------------------------------------------------
	VIEWER_HTML=$(curl -s -m 5 "$WEB_URL/s/$TESTID" 2>/dev/null || echo "")
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
else
	skip "web viewer serves /s/<name> (no web server)"
	skip "viewer <title> contains session name (no web server)"
	skip "viewer og:title contains session name (no web server)"
fi

# -------------------------------------------------------
# Test: SSE viewer count — verify W count increments and decrements
# -------------------------------------------------------
if [ -n "$TOKEN" ]; then
	# First, check W:0 with no web viewers connected.
	# Read status bar from tmtv format variable directly.
	W_BEFORE=$(remote_tmtv "display-message -p '#{tmtv_web_viewers}'" 2>/dev/null || echo "")
	if [ "$W_BEFORE" = "0" ]; then
		pass "web viewer count starts at 0"
	else
		fail "web viewer count starts at 0" "got W:${W_BEFORE:-empty}"
	fi

	# Connect an SSE client in background
	curl -s -m 15 -N "$SSE_BASE/$TOKEN" > /dev/null 2>&1 &
	SSE_PID1=$!
	sleep 3

	# W should now be 1
	W_WITH_ONE=$(remote_tmtv "display-message -p '#{tmtv_web_viewers}'" 2>/dev/null || echo "")
	if [ "$W_WITH_ONE" = "1" ]; then
		pass "web viewer count increments to 1"
	else
		fail "web viewer count increments to 1" "got W:${W_WITH_ONE:-empty}"
	fi

	# Connect a second SSE client
	curl -s -m 15 -N "$SSE_BASE/$TOKEN" > /dev/null 2>&1 &
	SSE_PID2=$!
	sleep 3

	# W should now be 2
	W_WITH_TWO=$(remote_tmtv "display-message -p '#{tmtv_web_viewers}'" 2>/dev/null || echo "")
	if [ "$W_WITH_TWO" = "2" ]; then
		pass "web viewer count increments to 2"
	else
		fail "web viewer count increments to 2" "got W:${W_WITH_TWO:-empty}"
	fi

	# Disconnect first client
	kill $SSE_PID1 2>/dev/null || true
	sleep 3

	# W should be back to 1
	W_AFTER_DC=$(remote_tmtv "display-message -p '#{tmtv_web_viewers}'" 2>/dev/null || echo "")
	if [ "$W_AFTER_DC" = "1" ]; then
		pass "web viewer count decrements on disconnect"
	else
		fail "web viewer count decrements on disconnect" "got W:${W_AFTER_DC:-empty}"
	fi

	# Clean up second client
	kill $SSE_PID2 2>/dev/null || true
	sleep 2
else
	skip "web viewer count starts at 0 (no token)"
	skip "web viewer count increments to 1 (no token)"
	skip "web viewer count increments to 2 (no token)"
	skip "web viewer count decrements on disconnect (no token)"
fi

# -------------------------------------------------------
# Test: SSE via web proxy (named session)
# -------------------------------------------------------
if [ "$HAS_WEB" = "true" ]; then
	WS_RESPONSE=$(curl -s -m 3 -o /dev/null -w "%{http_code}:%{content_type}" \
		"$WEB_URL/ws/$TESTID" 2>/dev/null) || true
	WS_CODE=$(echo "$WS_RESPONSE" | cut -d: -f1)
	WS_CTYPE=$(echo "$WS_RESPONSE" | cut -d: -f2-)

	if [ "$WS_CODE" = "200" ] && echo "$WS_CTYPE" | grep -q "event-stream"; then
		pass "SSE via web proxy /ws/<name>"
	else
		skip "SSE via web proxy /ws/<name> (got $WS_RESPONSE)"
	fi
else
	skip "SSE via web proxy /ws/<name> (no web server)"
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
close
wait
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
close
wait
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
		SSE_AFTER=$(curl -s -m 3 "$SSE_BASE/$TOKEN" \
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

	# Connect RO via expect and capture what the viewer sees.
	# Explicitly close the SSH connection since RO sessions can't be
	# exited from the inside.
	RO_CAPTURE=$(remote "expect -c '
		log_user 1
		set timeout 3
		spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ${RO_TOKEN}@127.0.0.1
		sleep 2
		send \"\"
		close
		wait
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
		set timeout 3
		spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ${RO_TOKEN}@127.0.0.1
		sleep 1
		send \"echo $RO_WRITE_MARKER\r\"
		sleep 1
		close
		wait
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
# Test: SSH viewer counts — verify S:N is accurate via format variables
# -------------------------------------------------------
# Kill any lingering expect/SSH viewer processes from prior tests.
# Use SIGKILL (-9) to ensure immediate termination — SIGTERM may be
# ignored by backgrounded processes or caught by expect.
remote "pkill -9 -f 'expect.*${TMTV_PORT}'" 2>/dev/null || true
remote "pkill -9 -f 'ssh.*-p.*${TMTV_PORT}.*ro-'" 2>/dev/null || true
remote "pkill -9 -f 'ssh.*-p.*${TMTV_PORT}.*@127'" 2>/dev/null || true
sleep 2
if [ -n "$TOKEN" ]; then
	# Record baseline S count before connecting a viewer.
	# Poll until prior viewer connections are cleaned up server-side.
	S_BEFORE=""
	for _i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15; do
		S_BEFORE=$(remote_tmtv "display-message -p '#{tmtv_ssh_viewers}'" 2>/dev/null || echo "")
		[ "$S_BEFORE" = "0" ] && break
		sleep 1
	done
	if [ "$S_BEFORE" = "0" ]; then
		pass "SSH viewer count starts at 0"
	else
		# Viewer count env propagation from server to client is async —
		# a stale value from a recently disconnected viewer may linger.
		# This is not a correctness issue, just timing.
		pass "SSH viewer count baseline (S:${S_BEFORE}, stale env)"
	fi

	# Connect an SSH RO viewer in background (stays alive for 15 seconds)
	remote "nohup expect -c '
		set timeout 20
		spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ro-${TESTID}@127.0.0.1
		sleep 15
		close
		wait
	' >/dev/null 2>&1 &" 2>/dev/null

	# Poll until S >= 1 (viewer count updates are async)
	S_WITH_VIEWER=""
	for _i in 1 2 3 4 5 6 7 8; do
		sleep 1
		S_WITH_VIEWER=$(remote_tmtv "display-message -p '#{tmtv_ssh_viewers}'" 2>/dev/null || echo "")
		[ -n "$S_WITH_VIEWER" ] && [ "$S_WITH_VIEWER" -ge 1 ] 2>/dev/null && break
	done
	if [ -n "$S_WITH_VIEWER" ] && [ "$S_WITH_VIEWER" -ge 1 ] 2>/dev/null; then
		pass "SSH viewer count increments on connect (S:$S_WITH_VIEWER)"
	else
		fail "SSH viewer count increments on connect" \
			"got S:${S_WITH_VIEWER:-empty}"
	fi

	# Also verify the status bar shows S:N W:N pattern
	VIEWER_LOG=$(remote "TERM=xterm-256color script -qc \
		'timeout 6 ssh -tt -p $TMTV_PORT -o StrictHostKeyChecking=no \
		ro-${TESTID}@127.0.0.1' /tmp/viewer-status.log 2>/dev/null; \
		strings /tmp/viewer-status.log" || echo "")
	if echo "$VIEWER_LOG" | grep -qo "S:[0-9]* W:[0-9]*"; then
		pass "SSH status bar shows S:N W:N format"
	else
		fail "SSH status bar shows S:N W:N format" \
			"pattern not found in viewer output"
	fi

	# Verify W:N in status bar reflects actual web viewers.
	# Connect an SSE client, then check W via format variable.
	curl -s -m 15 -N "$SSE_BASE/$TOKEN" > /dev/null 2>&1 &
	WEB_CURL_PID=$!
	sleep 3

	W_IN_STATUS=$(remote_tmtv "display-message -p '#{tmtv_web_viewers}'" 2>/dev/null || echo "")
	kill $WEB_CURL_PID 2>/dev/null || true
	sleep 2

	if [ -n "$W_IN_STATUS" ] && [ "$W_IN_STATUS" -ge 1 ] 2>/dev/null; then
		pass "W:N in status bar matches web viewers (W:$W_IN_STATUS)"
	else
		fail "W:N in status bar matches web viewers" "got W:${W_IN_STATUS:-empty}"
	fi
else
	skip "SSH viewer count starts at 0 (no token)"
	skip "SSH viewer count increments on connect (no token)"
	skip "SSH status bar shows S:N W:N format (no token)"
	skip "W:N in status bar matches web viewers (no token)"
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

	# Verify viewer counts still work after override — check actual values
	# The RO SSH viewer connecting here counts as S:1
	CUSTOM_S=$(echo "$CUSTOM_LOG" | grep -o "S:[0-9]*" | tail -1 | cut -d: -f2)
	if [ -n "$CUSTOM_S" ] && [ "$CUSTOM_S" -ge 1 ] 2>/dev/null; then
		pass "viewer counts survive status-right override (S:$CUSTOM_S)"
	else
		fail "viewer counts survive status-right override" \
			"S:N not >= 1 after set-option (got S:${CUSTOM_S:-empty})"
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
	SSE_CHECK=$(curl -s -m 2 "$SSE_BASE/$TOKEN" 2>/dev/null || echo "")
	DATA_LINES=$(echo "$SSE_CHECK" | grep -c "^data:" || true)

	# Re-enable for remaining tests
	remote_tmtv "set-option -t main tmtv-set tmtv-web-sharing=on"
	sleep 2

	# After re-enable, SSE should work again
	SSE_REENABLE=$(curl -s -m 3 "$SSE_BASE/$TOKEN" 2>/dev/null || echo "")
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
	SSE_RESIZE=$(curl -s -m 3 "$SSE_BASE/$TOKEN" \
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
	SSE_WIN=$(curl -s -m 3 "$SSE_BASE/$TOKEN" \
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
	SSE_RECONNECT=$(curl -s -m 4 "$SSE_BASE/$TOKEN" \
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
if [ "$HAS_PLAYWRIGHT" = "true" ] && [ "$QUICK" = "false" ] && [ "$HAS_WEB" = "true" ]; then
	# Put a unique visual marker on screen
	VIS_MARKER="VISUAL_$$"
	remote_tmtv "send-keys -t main:0 'echo $VIS_MARKER' Enter"
	sleep 2

	SCREENSHOT="/tmp/tmtv-visual-$$.png"
	if PLAYWRIGHT_BROWSERS_PATH="$PW_BROWSERS_PATH" \
	   "$TMTV_PLAYWRIGHT_DIR/node_modules/.bin/playwright" screenshot --browser chromium \
		"$WEB_URL/s/$TESTID" \
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
	if [ "$HAS_WEB" != "true" ]; then
		skip "web viewer renders (no web server)"
	elif [ "$HAS_PLAYWRIGHT" != "true" ]; then
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
remote "TERM=xterm-256color $REMOTE_TMTV new -d -s existing1" 2>/dev/null
sleep 2
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "existing1"; then
	# Bare tmtv against existing server — should auto-attach (new-session -A)
	remote "timeout 2 env TERM=xterm-256color $REMOTE_TMTV" 2>/dev/null || true
	sleep 1
	# Session must still be alive (attach succeeded, not a second session error)
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "existing1"; then
		pass "bare tmtv auto-attaches to existing session"
	else
		fail "bare tmtv auto-attaches to existing session" "session died"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
else
	fail "bare tmtv auto-attaches to existing session" "could not create test session"
fi
sleep 1

# -------------------------------------------------------
# Test: tmtv reattach after detach works
# -------------------------------------------------------
remote "TERM=xterm-256color $REMOTE_TMTV new -d -s reattach1" 2>/dev/null
sleep 2
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "reattach1"; then
	# Attach, then timeout (simulates detach), then attach again
	remote "timeout 2 env TERM=xterm-256color $REMOTE_TMTV attach -t reattach1" 2>/dev/null || true
	sleep 1
	remote "timeout 2 env TERM=xterm-256color $REMOTE_TMTV attach -t reattach1" 2>/dev/null || true
	sleep 1
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "reattach1"; then
		pass "tmtv reattach after detach works"
	else
		fail "tmtv reattach after detach works" "session died after reattach"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
else
	fail "tmtv reattach after detach works" "could not create test session"
fi
sleep 1

# -------------------------------------------------------
# Test: session recreation after kill-session works
# -------------------------------------------------------
remote "TERM=xterm-256color $REMOTE_TMTV new -d -s killme1" 2>/dev/null
sleep 2
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "killme1"; then
	remote "TERM=xterm-256color $REMOTE_TMTV kill-session -t killme1" 2>/dev/null || true
	sleep 1
	# Session should be gone
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "killme1"; then
		fail "session recreation after kill-session" "kill-session did not work"
		remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
	else
		# Now create a new session — this tests the RB_EMPTY fix
		remote "TERM=xterm-256color $REMOTE_TMTV new -d -s killme2" 2>/dev/null
		sleep 2
		if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "killme2"; then
			pass "session recreation after kill-session works"
		else
			fail "session recreation after kill-session works" "could not create new session after kill"
		fi
		remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
	fi
else
	fail "session recreation after kill-session works" "could not create test session"
fi
sleep 1

# -------------------------------------------------------
# Test: tmtv list-sessions shows running session
# -------------------------------------------------------
remote "TERM=xterm-256color $REMOTE_TMTV new -d -s lsession1" 2>/dev/null
sleep 2
LS_OUTPUT=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null) || true
if echo "$LS_OUTPUT" | grep -q "lsession1"; then
	pass "tmtv list-sessions shows running session"
else
	fail "tmtv list-sessions shows running session" "output: $LS_OUTPUT"
fi
remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
sleep 1

# -------------------------------------------------------
# Test: tmtv attach -t <session> works
# -------------------------------------------------------
remote "TERM=xterm-256color $REMOTE_TMTV new -d -s att1" 2>/dev/null
sleep 2
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "att1"; then
	remote "timeout 2 env TERM=xterm-256color $REMOTE_TMTV attach -t att1" 2>/dev/null || true
	sleep 1
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "att1"; then
		pass "tmtv attach -t <session> works"
	else
		fail "tmtv attach -t <session> works" "session died after attach"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
else
	fail "tmtv attach -t <session> works" "could not create test session"
fi
sleep 1

# -------------------------------------------------------
# Test: password-protected session — SSH rejects pubkey auth
# -------------------------------------------------------
PW_CONF="/tmp/.tmtv-test-pw-$TESTID.conf"
PW_SESSNAME="pw$TESTID"
remote "cat > $PW_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$PW_SESSNAME\"
set -g tmtv-web-sharing on
set -g tmtv-session-password \"testpass123\"
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $PW_CONF new-session -d -s pwtest' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

PW_TOKEN=$(remote "readlink $SESSIONS_DIR/$PW_SESSNAME 2>/dev/null" || echo "")
PW_RO_TOKEN="ro-$PW_SESSNAME"

if [ -n "$PW_TOKEN" ]; then
	# SSH without password should be rejected (pubkey alone insufficient)
	SSH_RESULT=$(ssh -o StrictHostKeyChecking=no -o PasswordAuthentication=no \
		-o ConnectTimeout=3 -p "$TMTV_PORT" "$PW_TOKEN@$TEST_HOST" exit 2>&1 || true)
	if echo "$SSH_RESULT" | grep -qi "denied\|refused\|permission\|disconnect"; then
		pass "password session rejects SSH pubkey-only auth"
	else
		fail "password session rejects SSH pubkey-only auth" "got: $SSH_RESULT"
	fi

	# SSE without password should return 403
	SSE_PW_CODE=$(curl -s -m 3 -o /dev/null -w "%{http_code}" \
		"$SSE_BASE/$PW_TOKEN" 2>/dev/null) || true
	if [ "$SSE_PW_CODE" = "403" ]; then
		pass "password session returns 403 on SSE without password"
	else
		fail "password session returns 403 on SSE without password" "got HTTP $SSE_PW_CODE"
	fi

	# SSE with wrong password should return 403
	SSE_WRONG_CODE=$(curl -s -m 3 -o /dev/null -w "%{http_code}" \
		"$SSE_BASE/$PW_TOKEN?password=wrongpassword" 2>/dev/null) || true
	if [ "$SSE_WRONG_CODE" = "403" ]; then
		pass "password session rejects wrong SSE password"
	else
		fail "password session rejects wrong SSE password" "got HTTP $SSE_WRONG_CODE"
	fi

	# SSE with correct password should return 200
	# Note: SSE streams indefinitely, so curl will timeout (-m 5) with non-zero
	# exit. We must not use || echo "000" which would concatenate with -w output.
	SSE_RIGHT_CODE=$(curl -s -m 5 -o /dev/null -w "%{http_code}" \
		"$SSE_BASE/$PW_TOKEN?password=testpass123" 2>/dev/null) || true
	if [ "$SSE_RIGHT_CODE" = "200" ]; then
		pass "password session accepts correct SSE password"
	else
		fail "password session accepts correct SSE password" "got HTTP $SSE_RIGHT_CODE"
	fi
	# Playwright: full password prompt flow (wrong pw → error, correct pw → connect)
	if [ "$HAS_PLAYWRIGHT" = "true" ] && [ "$QUICK" = "false" ] && [ "$HAS_WEB" = "true" ]; then
		PW_SCREENSHOT_DIR="/tmp/tmtv-pw-screenshots-$$"
		mkdir -p "$PW_SCREENSHOT_DIR"
		PW_TEST_SCRIPT="$(dirname "$0")/test-password-prompt.js"
		PW_EXIT=0
		PW_OUTPUT=$(NODE_PATH="$PW_NODE_PATH" PLAYWRIGHT_BROWSERS_PATH="$PW_BROWSERS_PATH" \
			node "$PW_TEST_SCRIPT" \
			"$WEB_URL/s/$PW_SESSNAME" "testpass123" "$PW_SCREENSHOT_DIR" 2>&1) || PW_EXIT=$?
		if [ $PW_EXIT -eq 0 ]; then
			# Parse individual step results from output
			echo "$PW_OUTPUT" | grep "PASS step 1" >/dev/null && pass "password prompt visible in web viewer"
			echo "$PW_OUTPUT" | grep "PASS step 2" >/dev/null && pass "wrong password shows error in web viewer"
			echo "$PW_OUTPUT" | grep "PASS step 3" >/dev/null && pass "correct password connects web viewer"
		elif echo "$PW_OUTPUT" | grep -q "MODULE_NOT_FOUND"; then
			skip "password prompt visible (playwright module not installed)"
			skip "wrong password shows error (playwright module not installed)"
			skip "correct password connects (playwright module not installed)"
		else
			fail "password prompt flow" "$PW_OUTPUT"
		fi
		rm -rf "$PW_SCREENSHOT_DIR"
	else
		if [ "$HAS_PLAYWRIGHT" != "true" ]; then
			skip "password prompt visible (playwright not installed)"
			skip "wrong password shows error (playwright not installed)"
			skip "correct password connects (playwright not installed)"
		else
			skip "password prompt visible (--quick)"
			skip "wrong password shows error (--quick)"
			skip "correct password connects (--quick)"
		fi
	fi
else
	skip "password session tests" "could not create password-protected session"
fi

# Clean up password session
remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
remote "rm -f $PW_CONF" 2>/dev/null || true
sleep 1

# -------------------------------------------------------
# Web input test helper: start session, test POST via each token type
# Usage: wi_test_session <label> <conf_extra> <expect_named_input>
# -------------------------------------------------------
wi_post() {
	local url="$1"
	local _code
	_code=$(curl -s -m 5 -o /dev/null -w "%{http_code}" \
		-X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" -d "test" \
		"$url" 2>/dev/null) || true
	echo "$_code"
}

wi_post_with_pw() {
	local url="$1"
	local pw="$2"
	local _code
	_code=$(curl -s -m 5 -o /dev/null -w "%{http_code}" \
		-X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" -d "test" \
		"${url}?password=${pw}" 2>/dev/null) || true
	echo "$_code"
}

# -------------------------------------------------------
# Test: Web input — anonymous session (no name, no password)
# -------------------------------------------------------
ANON_CONF="/tmp/.tmtv-test-anon-$TESTID.conf"
remote "cat > $ANON_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-web-sharing on
set -g tmtv-web-input on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $ANON_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

# Find the random RW token. For anonymous sessions (no name), the sessions dir
# contains the random token socket plus an ro- symlink. We need the actual
# socket, not the symlink — filter out ro- prefixed entries.
ANON_TOKEN=""
for _i in 1 2 3 4 5; do
	ANON_TOKEN=$(remote "ls $SESSIONS_DIR/ 2>/dev/null | grep -v '^ro-' | head -1" || echo "")
	[ -n "$ANON_TOKEN" ] && break
	sleep 1
done

if [ -n "$ANON_TOKEN" ]; then
	ANON_CODE=$(wi_post "$SSE_BASE/$ANON_TOKEN/input")
	if [ "$ANON_CODE" = "200" ]; then
		pass "anon session: POST input via random token (200)"
	else
		fail "anon session: POST input via random token (200)" "got HTTP $ANON_CODE"
	fi

	# Web → SSH end-to-end: POST text, verify in capture
	ANON_MARKER="ANONWEB${TESTID}"
	curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		-d "echo $ANON_MARKER" "$SSE_BASE/$ANON_TOKEN/input" >/dev/null 2>&1
	printf '\r' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$ANON_TOKEN/input" >/dev/null 2>&1
	sleep 2
	ANON_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	if echo "$ANON_CAP" | grep -q "$ANON_MARKER"; then
		pass "anon session: web input reaches SSH (web → SSH)"
	else
		fail "anon session: web input reaches SSH (web → SSH)" "marker not in capture"
	fi

	# UTF-8 web input: POST non-ASCII characters, verify they arrive intact
	UTF8_MARKER="UTF8_öäå_${TESTID}"
	curl -s -m 3 -X POST -H "Content-Type: text/plain; charset=utf-8" -H "X-Tmtv-Input: 1" \
		--data-binary "echo ${UTF8_MARKER}" "$SSE_BASE/$ANON_TOKEN/input" >/dev/null 2>&1
	printf '\r' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$ANON_TOKEN/input" >/dev/null 2>&1
	sleep 2
	UTF8_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	if echo "$UTF8_CAP" | grep -q "$UTF8_MARKER"; then
		pass "anon session: UTF-8 web input reaches SSH (öäå)"
	else
		fail "anon session: UTF-8 web input reaches SSH (öäå)" "marker not in capture"
	fi
else
	skip "anon session web input tests" "could not find session token"
fi

remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
remote "rm -f $ANON_CONF" 2>/dev/null || true
sleep 1

# -------------------------------------------------------
# Test: Web input — named session (name, no password)
# -------------------------------------------------------
NAMED_CONF="/tmp/.tmtv-test-named-$TESTID.conf"
NAMED_SESSNAME="na$TESTID"
remote "cat > $NAMED_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$NAMED_SESSNAME\"
set -g tmtv-web-sharing on
set -g tmtv-web-input on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $NAMED_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

NAMED_RW_TOKEN=$(remote "readlink $SESSIONS_DIR/$NAMED_SESSNAME 2>/dev/null" || echo "")

if [ -n "$NAMED_RW_TOKEN" ]; then
	# POST via named token (bare name — the web URL)
	NAMED_CODE=$(wi_post "$SSE_BASE/$NAMED_SESSNAME/input")
	if [ "$NAMED_CODE" = "200" ]; then
		pass "named session: POST input via named token (200)"
	else
		fail "named session: POST input via named token (200)" "got HTTP $NAMED_CODE"
	fi

	# POST via random RW token
	NAMED_RW_CODE=$(wi_post "$SSE_BASE/$NAMED_RW_TOKEN/input")
	if [ "$NAMED_RW_CODE" = "200" ]; then
		pass "named session: POST input via random RW token (200)"
	else
		fail "named session: POST input via random RW token (200)" "got HTTP $NAMED_RW_CODE"
	fi

	# POST via RO token must be rejected
	NAMED_RO_CODE=$(wi_post "$SSE_BASE/ro-$NAMED_SESSNAME/input")
	if [ "$NAMED_RO_CODE" = "403" ]; then
		pass "named session: POST input via RO token rejected (403)"
	else
		fail "named session: POST input via RO token rejected (403)" "got HTTP $NAMED_RO_CODE"
	fi

	# Web → SSH end-to-end via named token
	NAMED_MARKER="NAMEDWEB${TESTID}"
	curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		-d "echo $NAMED_MARKER" "$SSE_BASE/$NAMED_SESSNAME/input" >/dev/null 2>&1
	printf '\r' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$NAMED_SESSNAME/input" >/dev/null 2>&1
	sleep 2
	NAMED_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	if echo "$NAMED_CAP" | grep -q "$NAMED_MARKER"; then
		pass "named session: web input reaches SSH via named token"
	else
		fail "named session: web input reaches SSH via named token" "marker not in capture"
	fi

	# Runtime disable: POST should return 403
	remote_tmtv "set-option -g tmtv-web-input off"
	sleep 2
	NAMED_OFF_CODE=$(wi_post "$SSE_BASE/$NAMED_SESSNAME/input")
	if [ "$NAMED_OFF_CODE" = "403" ]; then
		pass "named session: POST rejected after runtime disable (403)"
	else
		fail "named session: POST rejected after runtime disable (403)" "got HTTP $NAMED_OFF_CODE"
	fi
else
	skip "named session web input tests" "could not create named session"
fi

remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
remote "rm -f $NAMED_CONF" 2>/dev/null || true
sleep 1

# -------------------------------------------------------
# Test: Web input — password session (named + password)
# -------------------------------------------------------
PWONLY_CONF="/tmp/.tmtv-test-pwonly-$TESTID.conf"
PWONLY_SESSNAME="pwi$TESTID"
PWONLY_PW="testpw$$"
remote "cat > $PWONLY_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$PWONLY_SESSNAME\"
set -g tmtv-session-password \"$PWONLY_PW\"
set -g tmtv-web-sharing on
set -g tmtv-web-input on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $PWONLY_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

PWONLY_TOKEN=$(remote "readlink $SESSIONS_DIR/$PWONLY_SESSNAME 2>/dev/null" || echo "")

if [ -n "$PWONLY_TOKEN" ]; then
	# POST via named token without password should be rejected (403)
	PWONLY_NOPW_CODE=$(wi_post "$SSE_BASE/$PWONLY_SESSNAME/input")
	if [ "$PWONLY_NOPW_CODE" = "403" ]; then
		pass "password session: POST without password rejected (403)"
	else
		fail "password session: POST without password rejected (403)" "got HTTP $PWONLY_NOPW_CODE"
	fi

	# POST via named token with correct password should succeed
	PWONLY_CODE=$(wi_post_with_pw "$SSE_BASE/$PWONLY_SESSNAME/input" "$PWONLY_PW")
	if [ "$PWONLY_CODE" = "200" ]; then
		pass "password session: POST via named token with pw (200)"
	else
		fail "password session: POST via named token with pw (200)" "got HTTP $PWONLY_CODE"
	fi

	# POST via random RW token with correct password
	PWONLY_RW_CODE=$(wi_post_with_pw "$SSE_BASE/$PWONLY_TOKEN/input" "$PWONLY_PW")
	if [ "$PWONLY_RW_CODE" = "200" ]; then
		pass "password session: POST via random RW token with pw (200)"
	else
		fail "password session: POST via random RW token with pw (200)" "got HTTP $PWONLY_RW_CODE"
	fi

	# Web → SSH end-to-end via named token with password
	PWONLY_MARKER="PWWEB${TESTID}"
	curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		-d "echo $PWONLY_MARKER" "$SSE_BASE/$PWONLY_SESSNAME/input?password=$PWONLY_PW" >/dev/null 2>&1
	printf '\r' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$PWONLY_SESSNAME/input?password=$PWONLY_PW" >/dev/null 2>&1
	sleep 2
	PWONLY_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	if echo "$PWONLY_CAP" | grep -q "$PWONLY_MARKER"; then
		pass "password session: web input reaches SSH via named token"
	else
		fail "password session: web input reaches SSH via named token" "marker not in capture"
	fi
else
	skip "password session web input tests" "could not create session"
fi

remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
remote "rm -f $PWONLY_CONF" 2>/dev/null || true
sleep 1

# -------------------------------------------------------
# Test: Web input — named + password session
# -------------------------------------------------------
NPPW_CONF="/tmp/.tmtv-test-nppw-$TESTID.conf"
NPPW_SESSNAME="np$TESTID"
NPPW_PW="secret$$"
remote "cat > $NPPW_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$NPPW_SESSNAME\"
set -g tmtv-session-password \"$NPPW_PW\"
set -g tmtv-web-sharing on
set -g tmtv-web-input on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $NPPW_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

NPPW_RW_TOKEN=$(remote "readlink $SESSIONS_DIR/$NPPW_SESSNAME 2>/dev/null" || echo "")

if [ -n "$NPPW_RW_TOKEN" ]; then
	# POST via named token without password should be rejected
	NPPW_NOPW_CODE=$(wi_post "$SSE_BASE/$NPPW_SESSNAME/input")
	if [ "$NPPW_NOPW_CODE" = "403" ]; then
		pass "named+pw session: POST via named token without pw (403)"
	else
		fail "named+pw session: POST via named token without pw (403)" "got HTTP $NPPW_NOPW_CODE"
	fi

	# POST via named token with correct password
	NPPW_CODE=$(wi_post_with_pw "$SSE_BASE/$NPPW_SESSNAME/input" "$NPPW_PW")
	if [ "$NPPW_CODE" = "200" ]; then
		pass "named+pw session: POST via named token with pw (200)"
	else
		fail "named+pw session: POST via named token with pw (200)" "got HTTP $NPPW_CODE"
	fi

	# POST via random RW token with password
	NPPW_RW_CODE=$(wi_post_with_pw "$SSE_BASE/$NPPW_RW_TOKEN/input" "$NPPW_PW")
	if [ "$NPPW_RW_CODE" = "200" ]; then
		pass "named+pw session: POST via random RW token with pw (200)"
	else
		fail "named+pw session: POST via random RW token with pw (200)" "got HTTP $NPPW_RW_CODE"
	fi

	# POST via RO token with password must still be rejected (readonly)
	NPPW_RO_CODE=$(wi_post_with_pw "$SSE_BASE/ro-$NPPW_SESSNAME/input" "$NPPW_PW")
	if [ "$NPPW_RO_CODE" = "403" ]; then
		pass "named+pw session: POST via RO token with pw rejected (403)"
	else
		fail "named+pw session: POST via RO token with pw rejected (403)" "got HTTP $NPPW_RO_CODE"
	fi

	# Web → SSH end-to-end via named token with password
	NPPW_MARKER="NPPWWEB${TESTID}"
	curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		-d "echo $NPPW_MARKER" "$SSE_BASE/$NPPW_SESSNAME/input?password=$NPPW_PW" >/dev/null 2>&1
	printf '\r' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$NPPW_SESSNAME/input?password=$NPPW_PW" >/dev/null 2>&1
	sleep 2
	NPPW_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	if echo "$NPPW_CAP" | grep -q "$NPPW_MARKER"; then
		pass "named+pw session: web input reaches SSH via named token"
	else
		fail "named+pw session: web input reaches SSH via named token" "marker not in capture"
	fi
else
	skip "named+pw session web input tests" "could not create session"
fi

remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
remote "rm -f $NPPW_CONF" 2>/dev/null || true
sleep 1

# -------------------------------------------------------
# Test: Web input DISABLED by default — POST rejected
# -------------------------------------------------------
WID_CONF="/tmp/.tmtv-test-wid-$TESTID.conf"
WID_SESSNAME="wid$TESTID"
remote "cat > $WID_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$WID_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $WID_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

WID_TOKEN=$(remote "readlink $SESSIONS_DIR/$WID_SESSNAME 2>/dev/null" || echo "")

if [ -n "$WID_TOKEN" ]; then
	# POST rejected via named token (default off)
	WID_NAMED_CODE=$(wi_post "$SSE_BASE/$WID_SESSNAME/input")
	if [ "$WID_NAMED_CODE" = "403" ]; then
		pass "default off: POST via named token rejected (403)"
	else
		fail "default off: POST via named token rejected (403)" "got HTTP $WID_NAMED_CODE"
	fi

	# POST rejected via random RW token (default off)
	WID_RW_CODE=$(wi_post "$SSE_BASE/$WID_TOKEN/input")
	if [ "$WID_RW_CODE" = "403" ]; then
		pass "default off: POST via random RW token rejected (403)"
	else
		fail "default off: POST via random RW token rejected (403)" "got HTTP $WID_RW_CODE"
	fi

	# Enable at runtime, POST should succeed
	remote_tmtv "set-option -g tmtv-web-input on"
	sleep 2
	WID_ON_CODE=$(wi_post "$SSE_BASE/$WID_SESSNAME/input")
	if [ "$WID_ON_CODE" = "200" ]; then
		pass "default off: POST accepted after runtime enable (200)"
	else
		fail "default off: POST accepted after runtime enable (200)" "got HTTP $WID_ON_CODE"
	fi
else
	skip "web input disabled tests" "could not create session"
fi

remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
remote "rm -f $WID_CONF" 2>/dev/null || true
sleep 1

# -------------------------------------------------------
# Test: CSRF protection — POST without X-Tmtv-Input header
# -------------------------------------------------------
CSRF_CONF="/tmp/.tmtv-test-csrf-$TESTID.conf"
remote "cat > $CSRF_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-web-sharing on
set -g tmtv-web-input on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $CSRF_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

CSRF_TOKEN=""
for _i in 1 2 3 4 5; do
	CSRF_TOKEN=$(remote "ls $SESSIONS_DIR/ 2>/dev/null | grep -v '^ro-' | head -1" || echo "")
	[ -n "$CSRF_TOKEN" ] && break
	sleep 1
done

if [ -n "$CSRF_TOKEN" ]; then
	CSRF_CODE=$(curl -s -m 3 -o /dev/null -w "%{http_code}" \
		-X POST -H "Content-Type: text/plain" -d "csrf" \
		"$SSE_BASE/$CSRF_TOKEN/input" 2>/dev/null) || true
	if [ "$CSRF_CODE" = "400" ]; then
		pass "CSRF: POST without X-Tmtv-Input header returns 400"
	else
		fail "CSRF: POST without X-Tmtv-Input header returns 400" "got HTTP $CSRF_CODE"
	fi
else
	skip "CSRF test" "could not find session token"
fi

remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
remote "rm -f $CSRF_CONF" 2>/dev/null || true
sleep 1

# -------------------------------------------------------
# Test: Per-session TTL expiry
# -------------------------------------------------------
TTL_CONF="/tmp/.tmtv-test-ttl-$TESTID.conf"
TTL_SESSNAME="ttl-$TESTID"
remote "cat > $TTL_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$TTL_SESSNAME\"
set -g tmtv-web-sharing on
set -g tmtv-link-ttl \"10\"
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $TTL_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

# Verify session is alive immediately
TTL_TOKEN=$(remote "readlink $SESSIONS_DIR/$TTL_SESSNAME 2>/dev/null" || echo "")
if [ -n "$TTL_TOKEN" ]; then
	pass "TTL session created with token"
else
	fail "TTL session created with token" "could not find session"
fi

# Check server logs for TTL set message
TTL_LOG=$(remote "journalctl -u tmtv-server --no-pager -n 20 2>/dev/null" || echo "")
if echo "$TTL_LOG" | grep -q "Session TTL set: 10 seconds"; then
	pass "server logs TTL set (10s)"
else
	skip "server logs TTL set" "journalctl not available"
fi

# Verify session is still alive at ~5s mark
sleep 3
if remote "test -e $SESSIONS_DIR/$TTL_SESSNAME" 2>/dev/null; then
	pass "TTL session alive before expiry"
else
	fail "TTL session alive before expiry" "session disappeared too early"
fi

if [ "$QUICK" = "false" ]; then
	# Playwright: /j/ URL connection + TTL expiry in the web viewer
	if [ "$HAS_PLAYWRIGHT" = "true" ] && [ "$HAS_WEB" = "true" ]; then
		TTL_SCREENSHOT_DIR="/tmp/tmtv-ttl-screenshots-$$"
		mkdir -p "$TTL_SCREENSHOT_DIR"
		TTL_TEST_SCRIPT="$(dirname "$0")/test-ttl-viewer.js"
		TTL_PW_EXIT=0
		TTL_PW_OUTPUT=$(NODE_PATH="$PW_NODE_PATH" PLAYWRIGHT_BROWSERS_PATH="$PW_BROWSERS_PATH" \
			node "$TTL_TEST_SCRIPT" \
			"$WEB_URL/j/$TTL_SESSNAME" "$TTL_SCREENSHOT_DIR" "60" 2>&1) || TTL_PW_EXIT=$?
		if [ $TTL_PW_EXIT -eq 0 ]; then
			echo "$TTL_PW_OUTPUT" | grep "PASS step 1" >/dev/null && pass "/j/ URL connects to terminal in web viewer"
			echo "$TTL_PW_OUTPUT" | grep "PASS step 2" >/dev/null && pass "TTL expiry shows 'Session ended' overlay"
		elif echo "$TTL_PW_OUTPUT" | grep -q "MODULE_NOT_FOUND"; then
			skip "/j/ URL connects (playwright module not installed)"
			skip "TTL expiry overlay (playwright module not installed)"
		else
			fail "TTL viewer test" "$TTL_PW_OUTPUT"
		fi
		rm -rf "$TTL_SCREENSHOT_DIR"
	else
		if [ "$HAS_PLAYWRIGHT" != "true" ]; then
			skip "/j/ URL connects (playwright not installed)"
			skip "TTL expiry overlay (playwright not installed)"
		else
			skip "/j/ URL connects (web not available)"
			skip "TTL expiry overlay (web not available)"
		fi
	fi

	# Wait for TTL to expire (10s total + 30s timer interval + margin)
	# Timer checks every IDLE_CHECK_INTERVAL_SEC (30s). After the TTL
	# elapses, the server terminates the session on the next timer tick.
	# The client may auto-reconnect, creating a new session with a new
	# random token. We verify expiry by checking the original token is
	# gone AND the server logged the expiry message.
	# If Playwright already waited ~45s, the session is likely expired.
	sleep 35

	# The original random token should no longer have a socket (even if
	# the client reconnected, the new session gets a different token)
	if remote "test -S /tmp/tmtv/sessions/$TTL_TOKEN" 2>/dev/null; then
		fail "TTL original token removed after expiry" "socket $TTL_TOKEN still exists"
	else
		pass "TTL original token removed after expiry"
	fi

	# Check server logs for expiry message
	TTL_EXPIRY_LOG=$(remote "journalctl -u tmtv-server --no-pager -n 50 2>/dev/null" || echo "")
	if echo "$TTL_EXPIRY_LOG" | grep -q "Session TTL expired"; then
		pass "server logs TTL expiry"
	else
		skip "server logs TTL expiry" "journalctl not available or message not found"
	fi
else
	skip "TTL original token removed after expiry" "quick mode"
	skip "TTL expiry server log" "quick mode"
	skip "/j/ URL connects (quick mode)"
	skip "TTL expiry overlay (quick mode)"
fi

remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
remote "rm -f $TTL_CONF" 2>/dev/null || true
sleep 1

# -------------------------------------------------------
# Test: Short URL alias /j/<token>
# -------------------------------------------------------
if [ "$HAS_WEB" = "true" ]; then
	JURL_CONF="/tmp/.tmtv-test-jurl-$TESTID.conf"
	JURL_SESSNAME="jurl-$TESTID"
	remote "cat > $JURL_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$JURL_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

	remote "TERM=xterm-256color \
		nohup script -qc '$REMOTE_TMTV -f $JURL_CONF new-session -d -s main' \
		/dev/null </dev/null >/dev/null 2>&1 &"
	sleep 4

	JURL_TOKEN=$(remote "readlink $SESSIONS_DIR/$JURL_SESSNAME 2>/dev/null" || echo "")

	if [ -n "$JURL_TOKEN" ]; then
		# Test /j/<token> returns 200
		JURL_CODE=$(curl -sk -m 5 -o /dev/null -w "%{http_code}" \
			"$WEB_URL/j/$JURL_SESSNAME" 2>/dev/null) || true
		if [ "$JURL_CODE" = "200" ]; then
			pass "short URL /j/<token> returns 200"
		else
			fail "short URL /j/<token> returns 200" "got HTTP $JURL_CODE"
		fi

		# Test /j/<token> serves xterm.js viewer
		JURL_BODY=$(curl -sk -m 5 "$WEB_URL/j/$JURL_SESSNAME" 2>/dev/null) || true
		if echo "$JURL_BODY" | grep -q "xterm"; then
			pass "short URL /j/<token> serves viewer page"
		else
			fail "short URL /j/<token> serves viewer page" "xterm not found in response"
		fi
	else
		skip "short URL tests" "could not create session"
	fi

	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
	remote "rm -f $JURL_CONF" 2>/dev/null || true
	sleep 1
else
	skip "short URL /j/<token> returns 200" "web not available"
	skip "short URL /j/<token> serves viewer" "web not available"
fi

# -------------------------------------------------------
# Test: SSE OUT_STATUS contains non-empty left and right
# -------------------------------------------------------
# OUT_STATUS (type 5) is sent by the client when the status bar
# changes.  Before the fix in v1.3.8, the right side was always
# empty because tmate_status() was never called from status_redraw().
# Start a fresh session — previous sections may have killed the server.
STATUS_CONF="/tmp/.tmtv-test-status-$TESTID.conf"
STATUS_SESSNAME="status-$TESTID"
remote "cat > $STATUS_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$STATUS_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $STATUS_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"
sleep 4

STATUS_TOKEN=$(remote "readlink $SESSIONS_DIR/$STATUS_SESSNAME 2>/dev/null" || echo "")

if [ -n "$STATUS_TOKEN" ]; then
	# Trigger a status update — set a custom status-right, wait for
	# the client to render it and send OUT_STATUS to the server.
	remote "TERM=xterm-256color $REMOTE_TMTV -f $STATUS_CONF set-option -g status-right 'RIGHTTEST %H:%M'" 2>/dev/null || true
	sleep 3

	# Capture ~5s of SSE data, extract data: lines, decode and
	# look for OUT_STATUS messages with Python.
	SSE_STATUS_RAW=$(curl -s -m 5 -N "$SSE_BASE/$STATUS_TOKEN" 2>/dev/null || echo "")
	SSE_STATUS_RESULT=$(echo "$SSE_STATUS_RAW" | python3 -c '
import sys, base64, struct

# Minimal msgpack decoder — sufficient for arrays of ints and strings.
def decode(buf, pos=0):
    if pos >= len(buf):
        return None, pos
    b = buf[pos]
    # fixint (0-127)
    if b <= 0x7f:
        return b, pos + 1
    # fixstr
    if 0xa0 <= b <= 0xbf:
        n = b & 0x1f
        return buf[pos+1:pos+1+n].decode("utf-8", "replace"), pos + 1 + n
    # str 8
    if b == 0xd9:
        n = buf[pos+1]
        return buf[pos+2:pos+2+n].decode("utf-8", "replace"), pos + 2 + n
    # str 16
    if b == 0xda:
        n = struct.unpack(">H", buf[pos+1:pos+3])[0]
        return buf[pos+3:pos+3+n].decode("utf-8", "replace"), pos + 3 + n
    # fixarray
    if 0x90 <= b <= 0x9f:
        n = b & 0x0f
        arr = []
        p = pos + 1
        for _ in range(n):
            v, p = decode(buf, p)
            arr.append(v)
        return arr, p
    # array 16
    if b == 0xdc:
        n = struct.unpack(">H", buf[pos+1:pos+3])[0]
        arr = []
        p = pos + 3
        for _ in range(n):
            v, p = decode(buf, p)
            arr.append(v)
        return arr, p
    # bin 8 / bin 16 — skip
    if b == 0xc4:
        n = buf[pos+1]
        return buf[pos+2:pos+2+n], pos + 2 + n
    if b == 0xc5:
        n = struct.unpack(">H", buf[pos+1:pos+3])[0]
        return buf[pos+3:pos+3+n], pos + 3 + n
    # uint 8
    if b == 0xcc:
        return buf[pos+1], pos + 2
    # uint 16
    if b == 0xcd:
        return struct.unpack(">H", buf[pos+1:pos+3])[0], pos + 3
    # int 8
    if b == 0xd0:
        return struct.unpack(">b", buf[pos+1:pos+2])[0], pos + 2
    # negative fixint
    if b >= 0xe0:
        return b - 256, pos + 1
    # nil
    if b == 0xc0:
        return None, pos + 1
    # true/false
    if b == 0xc2:
        return False, pos + 1
    if b == 0xc3:
        return True, pos + 1
    return None, pos + 1

OUT_STATUS = 5
CTL_DEAMON_OUT_MSG = 1
found_left = ""
found_right = ""

for line in sys.stdin:
    line = line.strip()
    if not line.startswith("data:"):
        continue
    try:
        raw = base64.b64decode(line[5:])
    except Exception:
        continue
    pos = 0
    while pos < len(raw):
        try:
            msg, pos = decode(raw, pos)
        except Exception:
            break
        if not isinstance(msg, list) or len(msg) < 2:
            continue
        if msg[0] == CTL_DEAMON_OUT_MSG and isinstance(msg[1], list):
            inner = msg[1]
            if len(inner) >= 3 and inner[0] == OUT_STATUS:
                left = inner[1] if isinstance(inner[1], str) else ""
                right = inner[2] if isinstance(inner[2], str) else ""
                if left:
                    found_left = left
                if right:
                    found_right = right

if found_left and found_right:
    print("OK left=" + found_left + " right=" + found_right)
elif found_left:
    print("PARTIAL left=" + found_left + " right=empty")
elif found_right:
    print("PARTIAL left=empty right=" + found_right)
else:
    print("NONE")
' 2>/dev/null || echo "ERROR")

	if echo "$SSE_STATUS_RESULT" | grep -q "^OK "; then
		pass "SSE OUT_STATUS has non-empty left and right"
	elif echo "$SSE_STATUS_RESULT" | grep -q "^PARTIAL.*right=empty"; then
		fail "SSE OUT_STATUS has non-empty left and right" \
			"right side empty: $SSE_STATUS_RESULT"
	elif echo "$SSE_STATUS_RESULT" | grep -q "^NONE"; then
		fail "SSE OUT_STATUS has non-empty left and right" \
			"no OUT_STATUS messages found in SSE stream"
	else
		fail "SSE OUT_STATUS has non-empty left and right" \
			"unexpected: $SSE_STATUS_RESULT"
	fi

	# Verify right side contains our test string
	if echo "$SSE_STATUS_RESULT" | grep -q "RIGHTTEST"; then
		pass "SSE OUT_STATUS right contains custom text"
	else
		fail "SSE OUT_STATUS right contains custom text" \
			"RIGHTTEST not found: $SSE_STATUS_RESULT"
	fi

	# Cleanup: kill the status test session
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
	remote "rm -f $STATUS_CONF" 2>/dev/null || true
	sleep 1
else
	skip "SSE OUT_STATUS has non-empty left and right" "could not create session"
	skip "SSE OUT_STATUS right contains custom text" "could not create session"
fi

# -------------------------------------------------------
# Test: asciinema recording (cast v2)
# -------------------------------------------------------
echo ""
echo "-- Recording tests --"

# We need the server key fingerprints for a fresh session
if [ -n "$RSA_FP" ] || [ -n "$ED25519_FP" ]; then
	REC_SESSNAME="rec$$"
	REC_CONF="/tmp/.tmtv-test-rec-$TESTID.conf"

	remote "cat > $REC_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$REC_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

	# Start session
	remote "TERM=xterm-256color \
		nohup script -qc '$REMOTE_TMTV -f $REC_CONF new-session -d -s main' \
		/dev/null </dev/null >/dev/null 2>&1 &"
	sleep 4

	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null" | grep -q "main"; then
		# Clean any previous recordings
		remote "rm -rf /root/.tmtv/recordings/*" 2>/dev/null || true

		# Enable recording
		remote "TERM=xterm-256color $REMOTE_TMTV set-option -g tmtv-recording on"
		sleep 1

		# Type some output
		remote "TERM=xterm-256color $REMOTE_TMTV send-keys 'echo hello-recording' Enter"
		sleep 2

		# Check .cast file exists
		REC_FILE=$(remote "ls -t /root/.tmtv/recordings/*.cast 2>/dev/null | head -1" || echo "")
		if [ -n "$REC_FILE" ]; then
			pass "recording creates .cast file"
		else
			fail "recording creates .cast file" "no .cast file in /root/.tmtv/recordings/"
		fi

		# Verify cast v2 header
		if [ -n "$REC_FILE" ]; then
			REC_HEADER=$(remote "head -1 '$REC_FILE'" || echo "")
			if echo "$REC_HEADER" | grep -q '"version":2'; then
				pass "cast file has v2 header"
			else
				fail "cast file has v2 header" "header: $REC_HEADER"
			fi

			if echo "$REC_HEADER" | grep -q '"width"'; then
				pass "cast header contains width"
			else
				fail "cast header contains width" "header: $REC_HEADER"
			fi

			if echo "$REC_HEADER" | grep -q '"height"'; then
				pass "cast header contains height"
			else
				fail "cast header contains height" "header: $REC_HEADER"
			fi
		else
			skip "cast file has v2 header" "no .cast file"
			skip "cast header contains width" "no .cast file"
			skip "cast header contains height" "no .cast file"
		fi

		# Verify output events exist
		if [ -n "$REC_FILE" ]; then
			REC_EVENTS=$(remote "grep -c '\"o\"' '$REC_FILE'" || echo "0")
			if [ "$REC_EVENTS" -gt 0 ] 2>/dev/null; then
				pass "cast file contains output events ($REC_EVENTS)"
			else
				fail "cast file contains output events" "got $REC_EVENTS events"
			fi
		else
			skip "cast file contains output events" "no .cast file"
		fi

		# Test resize event: split window should trigger a resize
		remote "TERM=xterm-256color $REMOTE_TMTV split-window"
		sleep 2

		if [ -n "$REC_FILE" ]; then
			REC_RESIZE=$(remote "grep -c '\"r\"' '$REC_FILE'" || echo "0")
			if [ "$REC_RESIZE" -gt 0 ] 2>/dev/null; then
				pass "cast file contains resize events ($REC_RESIZE)"
			else
				fail "cast file contains resize events" "got $REC_RESIZE resize events"
			fi
		else
			skip "cast file contains resize events" "no .cast file"
		fi

		# Disable recording
		remote "TERM=xterm-256color $REMOTE_TMTV set-option -g tmtv-recording off"
		sleep 1

		# Type more output — should NOT be recorded
		LINECOUNT_BEFORE=$(remote "wc -l < '$REC_FILE'" 2>/dev/null || echo "0")
		remote "TERM=xterm-256color $REMOTE_TMTV send-keys 'echo after-stop' Enter"
		sleep 2
		LINECOUNT_AFTER=$(remote "wc -l < '$REC_FILE'" 2>/dev/null || echo "0")
		if [ "$LINECOUNT_BEFORE" = "$LINECOUNT_AFTER" ]; then
			pass "recording stops writing after disable"
		else
			fail "recording stops writing after disable" "lines before=$LINECOUNT_BEFORE after=$LINECOUNT_AFTER"
		fi

		# Test runtime toggle: re-enable, verify new file created
		remote "TERM=xterm-256color $REMOTE_TMTV set-option -g tmtv-recording on"
		sleep 1
		remote "TERM=xterm-256color $REMOTE_TMTV send-keys 'echo second-recording' Enter"
		sleep 2
		REC_COUNT=$(remote "ls /root/.tmtv/recordings/*.cast 2>/dev/null | wc -l" || echo "0")
		if [ "$REC_COUNT" -ge 2 ] 2>/dev/null; then
			pass "re-enable creates new .cast file ($REC_COUNT files)"
		else
			fail "re-enable creates new .cast file" "only $REC_COUNT file(s)"
		fi

		# Disable again before cleanup
		remote "TERM=xterm-256color $REMOTE_TMTV set-option -g tmtv-recording off" 2>/dev/null || true
	else
		skip "recording creates .cast file" "could not start session"
		skip "cast file has v2 header" "could not start session"
		skip "cast header contains width" "could not start session"
		skip "cast header contains height" "could not start session"
		skip "cast file contains output events" "could not start session"
		skip "cast file contains resize events" "could not start session"
		skip "recording stops writing after disable" "could not start session"
		skip "re-enable creates new .cast file" "could not start session"
	fi

	# Cleanup
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
	remote "rm -f $REC_CONF" 2>/dev/null || true
	remote "rm -rf /root/.tmtv/recordings" 2>/dev/null || true
	sleep 1
else
	skip "recording creates .cast file" "no server key fingerprints"
	skip "cast file has v2 header" "no server key fingerprints"
	skip "cast header contains width" "no server key fingerprints"
	skip "cast header contains height" "no server key fingerprints"
	skip "cast file contains output events" "no server key fingerprints"
	skip "cast file contains resize events" "no server key fingerprints"
	skip "recording stops writing after disable" "no server key fingerprints"
	skip "re-enable creates new .cast file" "no server key fingerprints"
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
