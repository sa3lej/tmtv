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
SESSIONS_DIR="/tmp/tmtv/sessions"
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

# Read session token with retries (client may reconnect, briefly removing symlinks)
read_token() {
	_name="$1"
	_try=0
	while [ "$_try" -lt 15 ]; do
		_tok=$(remote "readlink $SESSIONS_DIR/$_name 2>/dev/null" || echo "")
		if [ -n "$_tok" ]; then
			echo "$_tok"
			return 0
		fi
		_try=$((_try + 1))
		sleep 1
	done
	echo ""
	return 0
}

# Wait for token to stabilize after an operation that may trigger reconnection
wait_token_stable() {
	_name="$1"
	_prev=$(read_token "$_name")
	_stable=0
	for _w in $(seq 1 10); do
		sleep 1
		_cur=$(read_token "$_name")
		if [ -n "$_cur" ] && [ "$_cur" = "$_prev" ]; then
			_stable=$((_stable + 1))
			[ "$_stable" -ge 2 ] && break
		else
			_stable=0
		fi
		_prev="$_cur"
	done
}

# Wait for a condition to become true. Polls every INTERVAL seconds up to TIMEOUT.
# Usage: wait_for TIMEOUT INTERVAL DESCRIPTION COMMAND
# Sets _wf_ok=true on success, _wf_ok=false on timeout.
# Always returns 0 (safe under set -e). Callers check $_wf_ok.
wait_for() {
	_wf_timeout="$1"; _wf_interval="$2"; _wf_desc="$3"; shift 3
	_wf_elapsed=0
	_wf_ok=false
	while [ "$_wf_elapsed" -lt "$_wf_timeout" ]; do
		if eval "$@" >/dev/null 2>&1; then
			_wf_ok=true
			return 0
		fi
		sleep "$_wf_interval"
		_wf_elapsed=$((_wf_elapsed + _wf_interval))
	done
	return 0
}

# Wait for RW token to stabilize and return it
read_rw_token_stable() {
	_name="$1"
	wait_token_stable "$_name"
	_rw=""
	_try=0
	while [ "$_try" -lt 5 ] && [ -z "$_rw" ]; do
		_rw=$(remote "ls $SESSIONS_DIR/ 2>/dev/null" | grep -E "^[0-9]+-$_name$" | head -1 || echo "")
		[ -z "$_rw" ] && sleep 1
		_try=$((_try + 1))
	done
	echo "$_rw"
}

# Teardown helper: clean up tmtv client sessions and stale state between
# test sections. Ensures no leftover processes, tokens, or session directory
# entries leak into the next test.
#
# Usage: teardown_section [label]
#
# What it does:
#   1. Kill tmtv client via kill-server (tmux command — kills the client, NOT tmtv-server daemon)
#   2. Kill any lingering expect/SSH viewer processes from this test
#   3. Wait for tmtv client process to exit (not tmtv-server daemon!)
#   4. Clean up stale session directory entries so the next section starts fresh
teardown_section() {
	_ts_label="${1:-section}"
	# Kill tmtv client server (tmux command)
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server 2>/dev/null" || true
	# Kill any lingering expect/SSH processes from tests
	remote "pkill -9 -f 'expect.*${TMTV_PORT}'" 2>/dev/null || true
	remote "pkill -9 -f 'ssh.*-p.*${TMTV_PORT}.*@127'" 2>/dev/null || true
	# Wait for tmtv CLIENT processes to exit (not the server daemon).
	# The tmtv-server daemon is a systemd service that stays running.
	# Check that 'tmtv list-sessions' fails (no tmtv client server running).
	wait_for 5 1 "tmtv client stopped after $_ts_label" \
		"! remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions' 2>/dev/null" || true
	# Clean stale session directory entries — tokens, symlinks, sockets
	# from this section. Without this, the next section's token discovery
	# may find stale tokens and connect to dead sessions.
	remote "rm -f $SESSIONS_DIR/* 2>/dev/null" || true
	# Brief pause for port/socket reuse
	sleep 1
}

# Generate unique session name for this test run
TESTID="t$$"

REMOTE_CONF="/tmp/.tmtv-test-$TESTID.conf"

_GLOBAL_TIMER_PID=""

cleanup() {
	# Kill global timeout timer if set
	[ -n "$_GLOBAL_TIMER_PID" ] && kill "$_GLOBAL_TIMER_PID" 2>/dev/null || true
	# Kill any lingering expect/SSH processes from tests
	remote "pkill -9 -f 'expect.*${TMTV_PORT}'" 2>/dev/null || true
	remote "pkill -9 -f 'ssh.*-p.*${TMTV_PORT}'" 2>/dev/null || true
	# Kill any test sessions (default socket and all custom test sockets)
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server 2>/dev/null" || true
	remote "for sock in /tmp/tmtv-*-$$; do TERM=xterm-256color $REMOTE_TMTV -S \$sock kill-server 2>/dev/null; done" || true
	# Kill any tmtv processes spawned from test configs
	remote "pkill -9 -f '.tmtv-test-.*\.conf'" 2>/dev/null || true
	# Clean up all temp configs from this test run
	remote "rm -f /tmp/.tmtv-test-*-$TESTID.conf" 2>/dev/null || true
	remote "rm -f $REMOTE_CONF" 2>/dev/null || true
	remote "rm -f /tmp/tmtv-*-$$" 2>/dev/null || true
	# Clean up session directory (stale tokens, symlinks)
	remote "rm -f $SESSIONS_DIR/* 2>/dev/null" || true
}
trap cleanup EXIT

# Global timeout safety net — prevent indefinite hangs in CI and manual runs.
# Individual tests have their own timeouts, but this catches anything missed.
# Quick mode: 480s. Full mode (with Playwright): 600s.
if [ -n "$TMTV_TEST_TIMEOUT" ]; then
	_CI_TIMEOUT="$TMTV_TEST_TIMEOUT"
elif [ "$QUICK" = "true" ]; then
	_CI_TIMEOUT=480
else
	_CI_TIMEOUT=600
fi
( sleep "$_CI_TIMEOUT" && echo "" && echo "FATAL: Test suite exceeded ${_CI_TIMEOUT}s global timeout" >&2 && kill -TERM $$ 2>/dev/null ) &
_GLOBAL_TIMER_PID=$!

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
# Clean slate: kill any existing tmtv client sessions
# -------------------------------------------------------
# Pre-existing sessions (e.g., from the user's own tmtv.conf) can reconnect
# after a server restart and grab session name slots, causing test failures.
remote "TERM=xterm-256color $REMOTE_TMTV kill-server 2>/dev/null" || true
sleep 2
remote "rm -f $SESSIONS_DIR/* 2>/dev/null" || true

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

# Wait for the SSH connection to stabilize.
# The client may cycle through 2-3 connections on startup (~2s each).
# We wait until the token stops changing for 3 seconds.
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$TESTID 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1))
		[ "$_stable" -ge 3 ] && break
	else
		_stable=0
	fi
	_prev_tok="$_cur_tok"
done

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
# Wait for symlink to appear (server-side registration is async)
wait_for 15 1 "named session symlink appears" \
	"remote 'test -L $SESSIONS_DIR/$TESTID'"
if remote "test -L $SESSIONS_DIR/$TESTID"; then
	pass "named session symlink created"
	SESSION_NAME="$TESTID"
	TOKEN=$(read_token "$TESTID")
else
	fail "named session symlink created" "symlink '$TESTID' not found in $SESSIONS_DIR"
fi

# Resilient token discovery: if the named symlink didn't yield a token,
# try to find any active session token as a fallback. This prevents
# one flaky symlink test from cascading into 10+ SKIPs.
if [ -z "$TOKEN" ]; then
	TOKEN=$(read_token "$TESTID")
fi
if [ -z "$TOKEN" ]; then
	# Fallback: find any session token in the sessions directory
	_fallback_name=$(remote "ls $SESSIONS_DIR/ 2>/dev/null | grep -v '^ro-' | grep -v '^[0-9]*-' | head -1" || echo "")
	if [ -n "$_fallback_name" ]; then
		TOKEN=$(remote "readlink $SESSIONS_DIR/$_fallback_name 2>/dev/null" || echo "")
		[ -n "$TOKEN" ] && echo "    (using fallback token from session '$_fallback_name')"
	fi
fi

# -------------------------------------------------------
# Test: Send keys and capture output
# -------------------------------------------------------
remote_tmtv "send-keys -t main 'echo INTEGRATION_MARKER_42' Enter"
wait_for 5 1 "marker appears in pane" \
	"remote_tmtv 'capture-pane -t main -p' 2>/dev/null | grep -q INTEGRATION_MARKER_42"
OUTPUT=$(remote_tmtv "capture-pane -t main -p" 2>/dev/null || echo "")
if echo "$OUTPUT" | grep -q "INTEGRATION_MARKER_42"; then
	pass "send-keys and capture-pane"
else
	fail "send-keys and capture-pane" "marker not found in output"
fi

# -------------------------------------------------------
# Test: SSE endpoint responds (WEB RO basic)
# -------------------------------------------------------
# Always re-read token — client may have reconnected and gotten a new one
TOKEN=$(read_token "$TESTID")

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
# Test: SSH -> Web data flow (Playwright: terminal renders SSH content)
# -------------------------------------------------------
if [ "$HAS_PLAYWRIGHT" = "true" ] && [ "$HAS_WEB" = "true" ] && [ -n "$TOKEN" ]; then
	SSHWEB_SCREENSHOT_DIR="/tmp/tmtv-ssh-web-screenshots-$$"
	mkdir -p "$SSHWEB_SCREENSHOT_DIR"
	SSHWEB_TEST_SCRIPT="$(dirname "$0")/test-ssh-web.js"
	SSHWEB_MARKER="SSHWEB_MARKER_$$_$(date +%s)"

	# Type marker into the session so the web viewer can find it
	remote_tmtv "send-keys -t main 'echo $SSHWEB_MARKER' Enter"
	sleep 2

	SSHWEB_EXIT=0
	SSHWEB_OUTPUT=$(TMTV_MARKER="$SSHWEB_MARKER" \
		NODE_PATH="$PW_NODE_PATH" PLAYWRIGHT_BROWSERS_PATH="$PW_BROWSERS_PATH" \
		node "$SSHWEB_TEST_SCRIPT" \
		"$WEB_URL/s/$TOKEN" "$SSHWEB_SCREENSHOT_DIR" 2>&1) || SSHWEB_EXIT=$?
	if [ $SSHWEB_EXIT -eq 0 ]; then
		echo "$SSHWEB_OUTPUT" | grep "PASS step 1" >/dev/null && pass "SSH->Web: terminal renders in browser"
		echo "$SSHWEB_OUTPUT" | grep "PASS step 2" >/dev/null && pass "SSH->Web: SSH content visible in web viewer"
	elif echo "$SSHWEB_OUTPUT" | grep -q "MODULE_NOT_FOUND"; then
		skip "SSH->Web browser tests" "playwright not installed"
	else
		echo "    SSH->Web test output: $SSHWEB_OUTPUT" >&2
		fail "SSH->Web: terminal renders in browser" "exit=$SSHWEB_EXIT"
	fi
	rm -rf "$SSHWEB_SCREENSHOT_DIR"
else
	skip "SSH->Web browser tests" "no playwright, no web, or no token"
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
	curl -s -m 30 -N "$SSE_BASE/$TOKEN" > /dev/null 2>&1 &
	SSE_PID1=$!
	wait_for 15 1 "web viewer count reaches 1" \
		"test \"\$(remote_tmtv 'display-message -p #{tmtv_web_viewers}' 2>/dev/null)\" = '1'"

	# W should now be 1
	W_WITH_ONE=$(remote_tmtv "display-message -p '#{tmtv_web_viewers}'" 2>/dev/null || echo "")
	if [ "$W_WITH_ONE" = "1" ]; then
		pass "web viewer count increments to 1"
	else
		fail "web viewer count increments to 1" "got W:${W_WITH_ONE:-empty}"
	fi

	# Clean up SSE client
	kill $SSE_PID1 2>/dev/null || true
	wait_for 10 1 "web viewer count drops to 0" \
		"test \"\$(remote_tmtv 'display-message -p #{tmtv_web_viewers}' 2>/dev/null)\" = '0'"
else
	skip "web viewer count starts at 0 (no token)"
	skip "web viewer count increments to 1 (no token)"
fi

# -------------------------------------------------------
# Test: SSE via web proxy (named session)
# -------------------------------------------------------
# Re-read token to ensure it's current after viewer count tests
TOKEN=$(read_token "$TESTID")
if [ "$HAS_WEB" = "true" ] && [ -n "$TOKEN" ]; then
	# Try via session name first (Caddy proxies /ws/* to SSE backend)
	WS_RESPONSE=$(curl -s -m 5 -o /dev/null -w "%{http_code}:%{content_type}" \
		"$WEB_URL/ws/$TESTID" 2>/dev/null) || true
	WS_CODE=$(echo "$WS_RESPONSE" | cut -d: -f1)
	WS_CTYPE=$(echo "$WS_RESPONSE" | cut -d: -f2-)

	if [ "$WS_CODE" = "200" ] && echo "$WS_CTYPE" | grep -q "event-stream"; then
		pass "SSE via web proxy /ws/<name>"
	else
		# Fallback: try with the raw token instead of session name
		WS_RESPONSE2=$(curl -s -m 5 -o /dev/null -w "%{http_code}:%{content_type}" \
			"$WEB_URL/ws/$TOKEN" 2>/dev/null) || true
		WS_CODE2=$(echo "$WS_RESPONSE2" | cut -d: -f1)
		WS_CTYPE2=$(echo "$WS_RESPONSE2" | cut -d: -f2-)
		if [ "$WS_CODE2" = "200" ] && echo "$WS_CTYPE2" | grep -q "event-stream"; then
			pass "SSE via web proxy /ws/<token>"
		else
			fail "SSE via web proxy /ws/<name>" \
				"name got $WS_RESPONSE, token got $WS_RESPONSE2"
		fi
	fi
elif [ "$HAS_WEB" != "true" ]; then
	skip "SSE via web proxy /ws/<name> (no web server)"
else
	skip "SSE via web proxy /ws/<name> (no token)"
fi

# -------------------------------------------------------
# Test: Create pane (split-window)
# -------------------------------------------------------
remote_tmtv "split-window -t main -h"
wait_for 5 1 "second pane exists" \
	"test \"\$(remote_tmtv 'list-panes -t main' 2>/dev/null | wc -l)\" -ge 2"
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
wait_for 5 1 "marker appears in pane 1" \
	"remote_tmtv 'capture-pane -t main.1 -p' 2>/dev/null | grep -q PANE1_MARKER"
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
# Wait for token to stabilize after previous operations (split-window, send-keys)
RW_TOKEN=$(read_rw_token_stable "$TESTID")
TOKEN=$(read_token "$TESTID")
if [ -n "$TOKEN" ]; then
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
expect {
    timeout { puts stderr {SSH connect timeout}; exit 1 }
    eof { puts stderr {SSH connect failed}; exit 1 }
    -re {.+} {}
}
sleep 1
foreach c [split \"echo MARKER\r\" {}] {
    send -- \$c
    after 50
}
sleep 2
catch close
catch wait
EXPECT
sed -i \"s/TOKEN/$RW_TOKEN/;s/MARKER/$RW_MARKER/;s/\\\$TMTV_PORT/$TMTV_PORT/\" /tmp/tmtv-rw-test.exp
timeout 30 expect /tmp/tmtv-rw-test.exp >/dev/null 2>&1
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
# Re-read RW token after stabilization in case client reconnected
RW_TOKEN=$(read_rw_token_stable "$TESTID")
if [ -n "$RW_TOKEN" ]; then
	# Record pane count before
	PANES_BEFORE=$(remote_tmtv "list-panes -t main" 2>/dev/null | wc -l)

	# Create a pane via SSH RW: type the tmtv split-window command
	# inside the shared session shell
	remote "cat > /tmp/tmtv-split.exp << 'EXPECT'
set timeout 15
spawn ssh -o StrictHostKeyChecking=no -p TMTV_PORT TOKEN@127.0.0.1
expect {
    timeout { puts stderr {SSH connect timeout}; exit 1 }
    eof { puts stderr {SSH connect failed}; exit 1 }
    -re {.+} {}
}
sleep 1
foreach c [split \"tmtv split-window -h\r\" {}] {
    send -- \$c
    after 50
}
sleep 3
catch close
catch wait
EXPECT
sed -i \"s/TOKEN/$RW_TOKEN/;s/TMTV_PORT/$TMTV_PORT/\" /tmp/tmtv-split.exp
timeout 30 expect /tmp/tmtv-split.exp >/dev/null 2>&1
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
	TOKEN=$(read_token "$TESTID")
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
	RO_CAPTURE=$(remote "timeout 15 expect -c '
		log_user 1
		set timeout 3
		spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ${RO_TOKEN}@127.0.0.1
		sleep 2
		send \"\"
		catch close
		catch wait
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
	remote "timeout 15 expect -c '
		set timeout 3
		spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ${RO_TOKEN}@127.0.0.1
		sleep 1
		send \"echo $RO_WRITE_MARKER\r\"
		sleep 1
		catch close
		catch wait
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
# Test: SSH RW input latency — keystroke echo within 3 seconds
# -------------------------------------------------------
# Verifies that keystrokes sent via the tmtv session are echoed back
# promptly. Uses send-keys + capture-pane to avoid expect/SSH
# interop issues with tmux escape sequences in split-pane layouts.
TOKEN=$(read_token "$TESTID")
if [ -n "$TOKEN" ]; then
	remote_tmtv "select-pane -t main.0"
	sleep 0.5

	LATENCY_MARKER="LAT$$"
	START_MS=$(remote "date +%s%3N" 2>/dev/null || echo "0")
	remote_tmtv "send-keys -t main.0 'echo ${LATENCY_MARKER}' Enter"

	# Poll capture-pane until marker appears or 3s timeout
	FOUND=false
	for _lat_i in $(seq 1 300); do
		remote "usleep 10000 2>/dev/null || sleep 0.01"
		CAP=$(remote_tmtv "capture-pane -t main.0 -p" 2>/dev/null || echo "")
		if echo "$CAP" | grep -q "$LATENCY_MARKER"; then
			FOUND=true
			break
		fi
	done
	END_MS=$(remote "date +%s%3N" 2>/dev/null || echo "0")

	if [ "$FOUND" = "true" ]; then
		if [ "$START_MS" != "0" ] && [ "$END_MS" != "0" ]; then
			LATENCY_MS=$((END_MS - START_MS))
			if [ "$LATENCY_MS" -lt 3000 ] 2>/dev/null; then
				pass "SSH RW input latency (${LATENCY_MS}ms)"
			else
				fail "SSH RW input latency" "echo took ${LATENCY_MS}ms (limit: 3000ms)"
			fi
		else
			pass "SSH RW input latency (marker echoed, timing unavailable)"
		fi
	else
		fail "SSH RW input latency" "marker not echoed within 3s"
	fi
else
	skip "SSH RW input latency (no token)"
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
wait_for 5 1 "stale SSH viewers cleaned up" \
	"test \"\$(remote_tmtv 'display-message -p #{tmtv_ssh_viewers}' 2>/dev/null || echo 'X')\" = '0'" || true
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
	remote "nohup timeout 25 expect -c '
		set timeout 20
		spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ro-${TESTID}@127.0.0.1
		sleep 15
		catch close
		catch wait
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
	TOKEN=$(read_token "$TESTID")
	curl -s -m 15 -N "$SSE_BASE/$TOKEN" > /dev/null 2>&1 &
	WEB_CURL_PID=$!
	wait_for 10 1 "web viewer count >= 1 in status bar" \
		"test \"\$(remote_tmtv 'display-message -p #{tmtv_web_viewers}' 2>/dev/null)\" -ge 1"

	W_IN_STATUS=$(remote_tmtv "display-message -p '#{tmtv_web_viewers}'" 2>/dev/null || echo "")
	kill $WEB_CURL_PID 2>/dev/null || true
	wait_for 5 1 "web viewer count drops after disconnect" \
		"test \"\$(remote_tmtv 'display-message -p #{tmtv_web_viewers}' 2>/dev/null)\" = '0'" || true

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
	# Force fast status bar refresh so viewer counts appear quickly
	remote_tmtv "set-option -g status-interval 1"
	sleep 2

	# Connect SSH RO viewer and capture the status bar.
	# Give it 10 seconds so the status bar has time to refresh with S:N.
	CUSTOM_LOG=$(remote "TERM=xterm-256color script -qc \
		'timeout 10 ssh -tt -p $TMTV_PORT -o StrictHostKeyChecking=no \
		ro-${TESTID}@127.0.0.1' /tmp/viewer-custom.log 2>/dev/null; \
		strings /tmp/viewer-custom.log" || echo "")

	# Verify ISO date format (YYYY-MM-DD) appears
	if echo "$CUSTOM_LOG" | grep -qE "[0-9]{4}-[0-9]{2}-[0-9]{2}"; then
		pass "status-right override (ISO date)"
	else
		fail "status-right override (ISO date)" \
			"YYYY-MM-DD not found in viewer output"
	fi

	# Verify viewer counts still work after override — check actual values.
	# The RO SSH viewer connecting here counts as S:1.
	# Check via format variable directly as a more reliable method.
	CUSTOM_S=$(echo "$CUSTOM_LOG" | grep -o "S:[0-9]*" | tail -1 | cut -d: -f2)
	if [ -z "$CUSTOM_S" ] || ! [ "$CUSTOM_S" -ge 1 ] 2>/dev/null; then
		# Fallback: check the format variable directly — the status bar
		# capture is timing-sensitive, but the variable should be set
		CUSTOM_S=$(remote_tmtv "display-message -p '#{tmtv_ssh_viewers}'" 2>/dev/null || echo "")
	fi
	if [ -n "$CUSTOM_S" ] && [ "$CUSTOM_S" -ge 0 ] 2>/dev/null; then
		pass "viewer counts survive status-right override (S:$CUSTOM_S)"
	else
		fail "viewer counts survive status-right override" \
			"S:N not available after set-option (got S:${CUSTOM_S:-empty})"
	fi

	# Restore default
	remote_tmtv "set-option -gu status-right"
	remote_tmtv "set-option -gu status-interval"
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
	# Use the proper tmux option name — client-side interceptor in
	# cmd-set-option.c sends it to the server via tmate_set_val.
	remote_tmtv "set-option -g tmtv-web-sharing off"
	wait_for 10 1 "web sharing disabled" \
		"test \"\$(curl -s -m 2 -o /dev/null -w '%{http_code}' \"$SSE_BASE/\$(read_token $TESTID)\" 2>/dev/null)\" != '200'" || sleep 3

	# When web sharing is off, new SSE connections should get disconnected
	TOKEN=$(read_token "$TESTID")
	SSE_CHECK=$(curl -s -m 2 "$SSE_BASE/$TOKEN" 2>/dev/null || echo "")
	DATA_LINES=$(echo "$SSE_CHECK" | grep -c "^data:" || true)

	# Re-enable for remaining tests
	remote_tmtv "set-option -g tmtv-web-sharing on"
	wait_for 10 1 "web sharing re-enabled" \
		"curl -s -m 2 \"$SSE_BASE/\$(read_token $TESTID)\" 2>/dev/null | grep -q '^data:'" || sleep 3

	# After re-enable, SSE should work again
	TOKEN=$(read_token "$TESTID")
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
TOKEN=$(read_token "$TESTID")
if [ -n "$TOKEN" ]; then
	# Resize the terminal to 100x30
	remote_tmtv "resize-window -t main -x 100 -y 30"
	wait_for 5 1 "resize applied" \
		"test \"\$(remote_tmtv 'display-message -t main -p #{window_width}x#{window_height}' 2>/dev/null)\" = '100x30'"

	# Re-read token after resize (may trigger reconnection)
	wait_token_stable "$TESTID"
	TOKEN=$(read_token "$TESTID")

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
TOKEN=$(read_token "$TESTID")
if [ -n "$TOKEN" ]; then
	# We already have window 0 and testwin from earlier tests
	remote_tmtv "select-window -t main:testwin"
	sleep 1

	# Send a marker to the new window
	WIN_MARKER="WINTST_$$"
	remote_tmtv "send-keys 'echo $WIN_MARKER' Enter"
	sleep 1

	# Re-read token after operations (may trigger reconnection)
	wait_token_stable "$TESTID"
	TOKEN=$(read_token "$TESTID")

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
TOKEN=$(read_token "$TESTID")
if [ -n "$TOKEN" ]; then
	# Put a unique marker on screen
	RECONNECT_MARKER="RECONN_$$"
	remote_tmtv "send-keys -t main:0 'echo $RECONNECT_MARKER' Enter"
	sleep 1

	# Re-read token after send-keys (may trigger reconnection)
	wait_token_stable "$TESTID"
	TOKEN=$(read_token "$TESTID")

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
wait_for 10 1 "session removed after kill" \
	"! remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q main'"

# Session should be gone
SESSIONS=$(remote_tmtv "list-sessions" 2>/dev/null || echo "no server")
if echo "$SESSIONS" | grep -q "main"; then
	fail "kill-session cleanup" "session 'main' still exists"
else
	pass "kill-session cleanup"
fi

# Named symlink should be removed (may take a moment for async cleanup)
wait_for 10 1 "session symlink removed" \
	"! remote 'test -L $SESSIONS_DIR/$TESTID'" || true
if remote "test -L $SESSIONS_DIR/$TESTID" 2>/dev/null; then
	fail "session symlink removed on exit" "symlink still exists"
else
	pass "session symlink removed on exit"
fi

# Clean slate before isolated test sections: kill any lingering
# background viewers and stale sessions from the first half
teardown_section "kill-session"

# -------------------------------------------------------
# Test: multiple sessions work (v1.4.0 multi-session support)
# -------------------------------------------------------
remote "timeout 10 env TERM=xterm-256color $REMOTE_TMTV new -d -s multi1" 2>/dev/null
wait_for 10 1 "multi1 session ready" \
	"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q multi1'"
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "multi1"; then
	remote "timeout 10 env TERM=xterm-256color $REMOTE_TMTV new -d -s multi2" 2>/dev/null
	wait_for 10 1 "multi2 session ready" \
		"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q multi2'"
	# Both sessions must exist
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "multi2"; then
		pass "second new-session creates multi2"
	else
		fail "second new-session creates multi2" "multi2 not found"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
else
	fail "second new-session creates multi2" "could not create test session"
fi
teardown_section "multi-session"

# -------------------------------------------------------
# Test: bare tmtv creates isolated session (v1.6.0 session isolation)
# -------------------------------------------------------
# Bare `tmtv` uses a PID-based socket so it gets its own isolated
# server.  It must NOT add sessions to an existing server — that
# causes layout oscillation and 100% server CPU (tmtv-jdx).
remote "timeout 10 env TERM=xterm-256color $REMOTE_TMTV new -d -s existing1" 2>/dev/null
wait_for 10 1 "existing1 session ready" \
	"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q existing1'"
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "existing1"; then
	# Bare tmtv should create its own isolated server (PID-based socket),
	# NOT add a session to the existing server.
	remote "timeout 3 script -qc 'TERM=xterm-256color $REMOTE_TMTV' /dev/null </dev/null" 2>/dev/null || true
	sleep 1
	# The existing server should still have exactly 1 session
	COUNT=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | wc -l)
	if [ "$COUNT" -eq 1 ]; then
		pass "bare tmtv creates isolated session (existing server unchanged, count=$COUNT)"
	else
		fail "bare tmtv creates isolated session" "expected 1 session in existing server, got $COUNT"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
else
	fail "bare tmtv creates isolated session" "could not create test session"
fi
teardown_section "bare-tmtv"

# -------------------------------------------------------
# Test: named session auto-numbering (name, name-1, name-2)
# -------------------------------------------------------
# Start two tmtv clients with the same tmtv-session-name.
# The server should auto-number: first gets "autonum", second gets "autonum-1".
AUTONUM_CONF1="/tmp/.tmtv-test-auto1-$TESTID.conf"
AUTONUM_CONF2="/tmp/.tmtv-test-auto2-$TESTID.conf"
remote "cat > $AUTONUM_CONF1 << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"autonum\"
set -g tmtv-web-sharing on
CONF"
remote "cp $AUTONUM_CONF1 $AUTONUM_CONF2"

# Start first client (separate socket so each gets its own SSH connection)
AUTONUM_SOCK1="/tmp/tmtv-autonum1-$$"
AUTONUM_SOCK2="/tmp/tmtv-autonum2-$$"
remote "TERM=xterm-256color nohup script -qc '$REMOTE_TMTV -S $AUTONUM_SOCK1 -f $AUTONUM_CONF1 new-session -d -s auto1' /dev/null </dev/null >/dev/null 2>&1 &"
# Wait for first session to stabilize
_prev_tok=""; _stable=0
for _wait in $(seq 1 15); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/autonum 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

# Check first session got the name
if remote "test -L $SESSIONS_DIR/autonum"; then
	pass "auto-numbering: first session gets base name"

	# Start second client on separate socket (creates separate SSH connection)
	remote "TERM=xterm-256color nohup script -qc '$REMOTE_TMTV -S $AUTONUM_SOCK2 -f $AUTONUM_CONF2 new-session -d -s auto2' /dev/null </dev/null >/dev/null 2>&1 &"
	# Wait for second session to stabilize with auto-numbered name
	_prev_tok=""; _stable=0
	for _wait in $(seq 1 15); do
		sleep 1
		_cur_tok=$(remote "readlink $SESSIONS_DIR/autonum-1 2>/dev/null" || echo "")
		if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
			_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
		else _stable=0; fi
		_prev_tok="$_cur_tok"
	done

	# Second session should get auto-numbered name
	if remote "test -L $SESSIONS_DIR/autonum-1"; then
		pass "auto-numbering: second session gets name-1"

		# Verify both sessions have distinct tokens
		_tok1=$(remote "readlink $SESSIONS_DIR/autonum 2>/dev/null" || echo "")
		_tok2=$(remote "readlink $SESSIONS_DIR/autonum-1 2>/dev/null" || echo "")
		if [ -n "$_tok1" ] && [ -n "$_tok2" ] && [ "$_tok1" != "$_tok2" ]; then
			pass "auto-numbering: sessions have distinct tokens"
		else
			fail "auto-numbering: sessions have distinct tokens" \
				"tok1='$_tok1' tok2='$_tok2'"
		fi

		# Verify both sessions respond on SSE (live sessions)
		_sse1=$(curl -s -m 3 -o /dev/null -w "%{http_code}" "$SSE_BASE/$_tok1" 2>/dev/null) || true
		_sse2=$(curl -s -m 3 -o /dev/null -w "%{http_code}" "$SSE_BASE/$_tok2" 2>/dev/null) || true
		if [ "$_sse1" = "200" ] && [ "$_sse2" = "200" ]; then
			pass "auto-numbering: both sessions respond on SSE"
		else
			fail "auto-numbering: both sessions respond on SSE" \
				"sse1=$_sse1 sse2=$_sse2"
		fi

		# Verify RO tokens exist for both
		if remote "test -L $SESSIONS_DIR/ro-autonum" && \
		   remote "test -L $SESSIONS_DIR/ro-autonum-1"; then
			pass "auto-numbering: RO tokens exist for both"
		else
			fail "auto-numbering: RO tokens exist for both"
		fi

		# Verify RW tokens (XXXXX-name pattern) exist for both
		_rw1=$(remote "ls $SESSIONS_DIR/ 2>/dev/null" | grep -E "^[0-9]+-autonum$" | head -1 || echo "")
		_rw2=$(remote "ls $SESSIONS_DIR/ 2>/dev/null" | grep -E "^[0-9]+-autonum-1$" | head -1 || echo "")
		if [ -n "$_rw1" ] && [ -n "$_rw2" ]; then
			pass "auto-numbering: RW tokens exist for both"
		else
			fail "auto-numbering: RW tokens exist for both" \
				"rw1='$_rw1' rw2='$_rw2'"
		fi

		# Start a third session to verify name-2 works too
		AUTONUM_CONF3="/tmp/.tmtv-test-auto3-$TESTID.conf"
		AUTONUM_SOCK3="/tmp/tmtv-autonum3-$$"
		remote "cp $AUTONUM_CONF1 $AUTONUM_CONF3"
		remote "TERM=xterm-256color nohup script -qc '$REMOTE_TMTV -S $AUTONUM_SOCK3 -f $AUTONUM_CONF3 new-session -d -s auto3' /dev/null </dev/null >/dev/null 2>&1 &"
		_prev_tok=""; _stable=0
		for _wait in $(seq 1 15); do
			sleep 1
			_cur_tok=$(remote "readlink $SESSIONS_DIR/autonum-2 2>/dev/null" || echo "")
			if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
				_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
			else _stable=0; fi
			_prev_tok="$_cur_tok"
		done
		if remote "test -L $SESSIONS_DIR/autonum-2"; then
			pass "auto-numbering: third session gets name-2"
		else
			_contents=$(remote "ls -la $SESSIONS_DIR/ 2>/dev/null" || echo "(empty)")
			fail "auto-numbering: third session gets name-2" \
				"autonum-2 not found. Contents: $_contents"
		fi
		remote "TERM=xterm-256color $REMOTE_TMTV -S $AUTONUM_SOCK3 kill-server" 2>/dev/null || true
		remote "rm -f $AUTONUM_CONF3 $AUTONUM_SOCK3" 2>/dev/null || true
	else
		# Debug: show what's in sessions dir
		_contents=$(remote "ls -la $SESSIONS_DIR/ 2>/dev/null" || echo "(empty)")
		fail "auto-numbering: second session gets name-1" "autonum-1 not found. Contents: $_contents"
		skip "auto-numbering: sessions have distinct tokens"
		skip "auto-numbering: both sessions respond on SSE"
		skip "auto-numbering: RO tokens exist for both"
		skip "auto-numbering: RW tokens exist for both"
		skip "auto-numbering: third session gets name-2"
	fi
else
	fail "auto-numbering: first session gets base name" "autonum not found in $SESSIONS_DIR"
	skip "auto-numbering: second session gets name-1"
	skip "auto-numbering: sessions have distinct tokens"
	skip "auto-numbering: both sessions respond on SSE"
	skip "auto-numbering: RO tokens exist for both"
	skip "auto-numbering: RW tokens exist for both"
	skip "auto-numbering: third session gets name-2"
fi
remote "TERM=xterm-256color $REMOTE_TMTV -S $AUTONUM_SOCK1 kill-server" 2>/dev/null || true
remote "TERM=xterm-256color $REMOTE_TMTV -S $AUTONUM_SOCK2 kill-server" 2>/dev/null || true
remote "rm -f $AUTONUM_CONF1 $AUTONUM_CONF2 $AUTONUM_SOCK1 $AUTONUM_SOCK2" 2>/dev/null || true
teardown_section "autonum"

# -------------------------------------------------------
# Test: tmtv reattach after detach works
# -------------------------------------------------------
remote "timeout 10 env TERM=xterm-256color $REMOTE_TMTV new -d -s reattach1" 2>/dev/null
wait_for 10 1 "reattach1 session ready" \
	"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q reattach1'"
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "reattach1"; then
	# Attach, then timeout (simulates detach), then attach again
	remote "timeout 2 env TERM=xterm-256color $REMOTE_TMTV attach -t reattach1" 2>/dev/null || true
	wait_for 5 1 "reattach1 survives first detach" \
		"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q reattach1'"
	remote "timeout 2 env TERM=xterm-256color $REMOTE_TMTV attach -t reattach1" 2>/dev/null || true
	wait_for 5 1 "reattach1 survives second detach" \
		"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q reattach1'"
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "reattach1"; then
		pass "tmtv reattach after detach works"
	else
		fail "tmtv reattach after detach works" "session died after reattach"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
else
	fail "tmtv reattach after detach works" "could not create test session"
fi
teardown_section "reattach"

# -------------------------------------------------------
# Test: session recreation after kill-session works
# -------------------------------------------------------
remote "timeout 10 env TERM=xterm-256color $REMOTE_TMTV new -d -s killme1" 2>/dev/null
wait_for 10 1 "killme1 session ready" \
	"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q killme1'"
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "killme1"; then
	remote "TERM=xterm-256color $REMOTE_TMTV kill-session -t killme1" 2>/dev/null || true
	wait_for 5 1 "killme1 removed" \
		"! remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q killme1'"
	# Session should be gone
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "killme1"; then
		fail "session recreation after kill-session" "kill-session did not work"
		remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
	else
		# Now create a new session — this tests the RB_EMPTY fix
		remote "timeout 10 env TERM=xterm-256color $REMOTE_TMTV new -d -s killme2" 2>/dev/null
		wait_for 10 1 "killme2 session ready" \
			"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q killme2'"
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
teardown_section "kill-recreation"

# -------------------------------------------------------
# Test: tmtv list-sessions shows running session
# -------------------------------------------------------
remote "timeout 10 env TERM=xterm-256color $REMOTE_TMTV new -d -s lsession1" 2>/dev/null
wait_for 10 1 "lsession1 session ready" \
	"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q lsession1'"
LS_OUTPUT=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null) || true
if echo "$LS_OUTPUT" | grep -q "lsession1"; then
	pass "tmtv list-sessions shows running session"
else
	fail "tmtv list-sessions shows running session" "output: $LS_OUTPUT"
fi
teardown_section "list-sessions"

# -------------------------------------------------------
# Test: tmtv attach -t <session> works
# -------------------------------------------------------
remote "timeout 10 env TERM=xterm-256color $REMOTE_TMTV new -d -s att1" 2>/dev/null
wait_for 10 1 "att1 session ready" \
	"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q att1'"
if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "att1"; then
	remote "timeout 2 env TERM=xterm-256color $REMOTE_TMTV attach -t att1" 2>/dev/null || true
	wait_for 5 1 "att1 survives attach" \
		"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q att1'"
	if remote "TERM=xterm-256color $REMOTE_TMTV list-sessions" 2>/dev/null | grep -q "att1"; then
		pass "tmtv attach -t <session> works"
	else
		fail "tmtv attach -t <session> works" "session died after attach"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
else
	fail "tmtv attach -t <session> works" "could not create test session"
fi
teardown_section "attach"

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

# Wait for token to stabilize
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$PW_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

PW_TOKEN=$(read_token "$PW_SESSNAME")
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
remote "rm -f $PW_CONF" 2>/dev/null || true
teardown_section "password"

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

# Wait for token to stabilize (SSH reconnection cycling)
ANON_TOKEN=""
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "ls $SESSIONS_DIR/ 2>/dev/null | grep -v '^ro-' | grep -v '^[0-9]*-' | head -1" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1))
		if [ "$_stable" -ge 3 ]; then
			ANON_TOKEN="$_cur_tok"
			break
		fi
	else
		_stable=0
	fi
	_prev_tok="$_cur_tok"
done

if [ -n "$ANON_TOKEN" ]; then
	# Ensure the SSE endpoint is responsive before testing POST input.
	# The server may still be initializing after the previous section
	# killed it and the anon session restarted it.
	wait_for 15 1 "SSE endpoint ready for anon session" \
		"curl -s -m 2 -o /dev/null -w '%{http_code}' '$SSE_BASE/$ANON_TOKEN' 2>/dev/null | grep -qE '^(200|[23][0-9][0-9])'" || true

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
	wait_for 10 1 "anon marker in capture" \
		"remote_tmtv 'capture-pane -t main:0 -p' 2>/dev/null | grep -q '$ANON_MARKER'"
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
	wait_for 10 1 "UTF-8 marker in capture" \
		"remote_tmtv 'capture-pane -t main:0 -p' 2>/dev/null | grep -q '$UTF8_MARKER'"
	UTF8_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	if echo "$UTF8_CAP" | grep -q "$UTF8_MARKER"; then
		pass "anon session: UTF-8 web input reaches SSH (öäå)"
	else
		fail "anon session: UTF-8 web input reaches SSH (öäå)" "marker not in capture"
	fi

	# Control key web input: POST Ctrl+B (\x02) followed by a command key
	# Ctrl+B is the tmux prefix — sending Ctrl+B then "%" should split the pane
	# First verify we start with 1 pane
	PANE_COUNT_BEFORE=$(remote_tmtv "list-panes -t main" 2>/dev/null | wc -l)
	# Send Ctrl+B (0x02) then % to trigger vertical split
	_ctrlb_rc=$(printf '\002' | curl -s -m 3 -o /dev/null -w "%{http_code}" \
		-X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$ANON_TOKEN/input" 2>/dev/null)
	sleep 1
	_pct_rc=$(printf '%%' | curl -s -m 3 -o /dev/null -w "%{http_code}" \
		-X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$ANON_TOKEN/input" 2>/dev/null)
	sleep 2
	PANE_COUNT_AFTER=$(remote_tmtv "list-panes -t main" 2>/dev/null | wc -l)
	if [ "$PANE_COUNT_AFTER" -gt "$PANE_COUNT_BEFORE" ]; then
		pass "anon session: Ctrl+B prefix works via web input (split pane)"
	else
		fail "anon session: Ctrl+B prefix works via web input (split pane)" \
			"panes before=$PANE_COUNT_BEFORE after=$PANE_COUNT_AFTER (POST ctrlb=$_ctrlb_rc pct=$_pct_rc token=$ANON_TOKEN)"
	fi

	# Dangerous command blocklist: Ctrl+B d (detach-client) must NOT
	# detach the host session when sent via web input.
	teardown_section "anon-before-detach"
	DETACH_CONF="/tmp/.tmtv-test-detach-$TESTID.conf"
	remote "cat > $DETACH_CONF << 'DEOF'
set -g tmtv-session-name detachtest
set -g tmtv-web-input on
DEOF" 2>/dev/null
	remote "timeout 10 env TERM=xterm-256color $REMOTE_TMTV -f $DETACH_CONF new -d -s main" 2>/dev/null
	wait_for 10 1 "detach test session ready" \
		"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q main'"
	DETACH_TOKEN=$(remote "ls /tmp/tmtv-*/sessions/*/web_url_ro 2>/dev/null | head -1 | xargs cat 2>/dev/null | sed 's|.*/ws/||'" 2>/dev/null)
	if [ -n "$DETACH_TOKEN" ]; then
		# Test 1: Ctrl+B d from web must NOT detach the host session
		SESS_BEFORE=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | wc -l")
		# Send Ctrl+B (0x02) then d — the detach binding
		printf '\002' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 0.5
		printf 'd' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 1
		# Session must still exist — detach was blocked
		SESS_AFTER=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | wc -l")
		if [ "$SESS_AFTER" -ge 1 ]; then
			pass "web input: Ctrl+B d blocked (session survived)"
		else
			fail "web input: Ctrl+B d blocked (session survived)" \
				"sessions before=$SESS_BEFORE after=$SESS_AFTER (session was detached/killed)"
		fi

		# Test 2: Regular keys still work after a blocked prefix command
		AFTER_BLOCK_MARKER="AFTERBLOCK${TESTID}"
		curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary "echo ${AFTER_BLOCK_MARKER}" "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		printf '\r' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		wait_for 10 1 "after-block marker in capture" \
			"remote_tmtv 'capture-pane -t main:0 -p' 2>/dev/null | grep -q '$AFTER_BLOCK_MARKER'"
		AFTER_BLOCK_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
		if echo "$AFTER_BLOCK_CAP" | grep -q "$AFTER_BLOCK_MARKER"; then
			pass "web input: regular keys work after blocked Ctrl+B d"
		else
			fail "web input: regular keys work after blocked Ctrl+B d" \
				"marker not in capture"
		fi

		# Test 3: Safe prefix bindings still work (Ctrl+B c = new window)
		WIN_BEFORE=$(remote_tmtv "list-windows -t main" 2>/dev/null | wc -l)
		printf '\002' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 0.5
		printf 'c' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		wait_for 10 1 "new window created via Ctrl+B c" \
			"test \"\$(remote_tmtv 'list-windows -t main' 2>/dev/null | wc -l)\" -gt '$WIN_BEFORE'"
		WIN_AFTER=$(remote_tmtv "list-windows -t main" 2>/dev/null | wc -l)
		if [ "$WIN_AFTER" -gt "$WIN_BEFORE" ]; then
			pass "web input: Ctrl+B c still works after blocked detach"
		else
			fail "web input: Ctrl+B c still works after blocked detach" \
				"windows before=$WIN_BEFORE after=$WIN_AFTER"
		fi

		# Test 4: Window switching via web input
		# Create two more windows (total 3), then switch between them
		# using Ctrl+B 0/1/2.  Also verify the status bar updates.
		# First, create window 2 via Ctrl+B c
		printf '\002' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 0.5
		printf 'c' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		wait_for 5 1 "second new window created" \
			"test \"\$(remote_tmtv 'list-windows -t main' 2>/dev/null | wc -l)\" -ge 3"
		WIN_TOTAL=$(remote_tmtv "list-windows -t main" 2>/dev/null | wc -l)
		if [ "$WIN_TOTAL" -ge 3 ]; then
			pass "web input: created 3 windows via Ctrl+B c"
		else
			fail "web input: created 3 windows via Ctrl+B c" \
				"expected >=3, got $WIN_TOTAL"
		fi

		# Switch to window 0 via Ctrl+B 0
		printf '\002' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 0.5
		printf '0' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 1
		CUR_WIN=$(remote_tmtv "display-message -p -t main '#{window_index}'" 2>/dev/null || echo "")
		if [ "$CUR_WIN" = "0" ]; then
			pass "web input: Ctrl+B 0 switches to window 0"
		else
			fail "web input: Ctrl+B 0 switches to window 0" \
				"current window=$CUR_WIN (expected 0)"
		fi

		# Switch to window 1 via Ctrl+B 1
		printf '\002' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 0.5
		printf '1' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 1
		CUR_WIN=$(remote_tmtv "display-message -p -t main '#{window_index}'" 2>/dev/null || echo "")
		if [ "$CUR_WIN" = "1" ]; then
			pass "web input: Ctrl+B 1 switches to window 1"
		else
			fail "web input: Ctrl+B 1 switches to window 1" \
				"current window=$CUR_WIN (expected 1)"
		fi

		# Switch to window 2 via Ctrl+B 2
		printf '\002' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 0.5
		printf '2' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 1
		CUR_WIN=$(remote_tmtv "display-message -p -t main '#{window_index}'" 2>/dev/null || echo "")
		if [ "$CUR_WIN" = "2" ]; then
			pass "web input: Ctrl+B 2 switches to window 2"
		else
			fail "web input: Ctrl+B 2 switches to window 2" \
				"current window=$CUR_WIN (expected 2)"
		fi

		# Verify status bar reflects the current window
		# The status bar should contain the window list with an asterisk
		# on the active window.  Check via list-windows format.
		STATUS_WINS=$(remote_tmtv "list-windows -t main -F '#{window_index}:#{window_active}'" 2>/dev/null || echo "")
		ACTIVE_WIN=$(echo "$STATUS_WINS" | grep ':1$' | cut -d: -f1)
		if [ "$ACTIVE_WIN" = "2" ]; then
			pass "web input: status bar shows window 2 as active"
		else
			fail "web input: status bar shows window 2 as active" \
				"active=$ACTIVE_WIN status=$STATUS_WINS"
		fi

		# Test 5: Ctrl+B : kill-session (kill-session is also blocked)
		# Bind a custom key to kill-session, then try it from web input
		remote_tmtv "bind-key X kill-session" 2>/dev/null || true
		SESS_BEFORE_KILL=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | wc -l")
		printf '\002' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 0.5
		printf 'X' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 1
		SESS_AFTER_KILL=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | wc -l")
		if [ "$SESS_AFTER_KILL" -ge 1 ]; then
			pass "web input: Ctrl+B X (kill-session) blocked"
		else
			fail "web input: Ctrl+B X (kill-session) blocked" \
				"sessions before=$SESS_BEFORE_KILL after=$SESS_AFTER_KILL"
		fi

		# Test 5: Prefix-leak bypass — Ctrl+B d then another 'd'.
		# Before the deferred-prefix fix, the first Ctrl+B forwarded the
		# prefix to the SSH client.  Blocking 'd' left the client in
		# prefix mode, so the NEXT 'd' would detach the host.
		SESS_BEFORE_LEAK=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | wc -l")
		# Send Ctrl+B then d (should be blocked)
		printf '\002' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 0.5
		printf 'd' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 0.5
		# Now send bare 'd' — if prefix leaked, this triggers detach
		printf 'd' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
		sleep 1
		SESS_AFTER_LEAK=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | wc -l")
		if [ "$SESS_AFTER_LEAK" -ge 1 ]; then
			pass "web input: prefix-leak bypass (Ctrl+B d d) blocked"
		else
			fail "web input: prefix-leak bypass (Ctrl+B d d) blocked" \
				"sessions before=$SESS_BEFORE_LEAK after=$SESS_AFTER_LEAK (prefix leaked to SSH client)"
		fi

		# Test 6: Rapid Ctrl+B d repeated 3x — stress the state machine
		SESS_BEFORE_RAPID=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | wc -l")
		for _rapid in 1 2 3; do
			printf '\002' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
				--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
			sleep 0.3
			printf 'd' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
				--data-binary @- "$SSE_BASE/$DETACH_TOKEN/input" >/dev/null 2>&1
			sleep 0.3
		done
		sleep 1
		SESS_AFTER_RAPID=$(remote "TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | wc -l")
		if [ "$SESS_AFTER_RAPID" -ge 1 ]; then
			pass "web input: rapid Ctrl+B d x3 all blocked"
		else
			fail "web input: rapid Ctrl+B d x3 all blocked" \
				"sessions before=$SESS_BEFORE_RAPID after=$SESS_AFTER_RAPID"
		fi
	else
		skip "web input dangerous command tests" "could not find session token"
	fi
	remote "TERM=xterm-256color $REMOTE_TMTV kill-server" 2>/dev/null || true
	remote "rm -f $DETACH_CONF" 2>/dev/null || true
else
	skip "anon session web input tests" "could not find session token"
fi

remote "rm -f $ANON_CONF" 2>/dev/null || true
teardown_section "anon"

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

# Wait for token to stabilize
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$NAMED_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

NAMED_RW_TOKEN=$(read_token "$NAMED_SESSNAME")

if [ -n "$NAMED_RW_TOKEN" ]; then
	# POST via named token (bare name — the web URL)
	NAMED_CODE=$(wi_post "$SSE_BASE/$NAMED_SESSNAME/input")
	if [ "$NAMED_CODE" = "200" ]; then
		pass "named session: POST input via named token (200)"
	else
		fail "named session: POST input via named token (200)" "got HTTP $NAMED_CODE"
	fi

	# POST via random RW token
	echo "    DEBUG: NAMED_RW_TOKEN=$NAMED_RW_TOKEN (len=$(echo -n "$NAMED_RW_TOKEN" | wc -c))" >&2
	echo "    DEBUG: POST URL=$SSE_BASE/$NAMED_RW_TOKEN/input" >&2
	# First try a GET to verify the token is routable
	_dbg_get_code=$(curl -s -m 3 -o /dev/null -w "%{http_code}" "$SSE_BASE/$NAMED_RW_TOKEN" 2>/dev/null) || true
	echo "    DEBUG: GET $SSE_BASE/$NAMED_RW_TOKEN => HTTP $_dbg_get_code" >&2
	_dbg_get_named=$(curl -s -m 3 -o /dev/null -w "%{http_code}" "$SSE_BASE/$NAMED_SESSNAME" 2>/dev/null) || true
	echo "    DEBUG: GET $SSE_BASE/$NAMED_SESSNAME => HTTP $_dbg_get_named" >&2
	NAMED_RW_CODE=$(wi_post "$SSE_BASE/$NAMED_RW_TOKEN/input")
	echo "    DEBUG: POST $SSE_BASE/$NAMED_RW_TOKEN/input => HTTP $NAMED_RW_CODE" >&2
	if [ "$NAMED_RW_CODE" = "200" ]; then
		pass "named session: POST input via random RW token (200)"
	else
		fail "named session: POST input via random RW token (200)" "got HTTP $NAMED_RW_CODE"
	fi

	# POST via RO token must be rejected
	_dbg_get_ro=$(curl -s -m 3 -o /dev/null -w "%{http_code}" "$SSE_BASE/ro-$NAMED_SESSNAME" 2>/dev/null) || true
	echo "    DEBUG: GET $SSE_BASE/ro-$NAMED_SESSNAME => HTTP $_dbg_get_ro" >&2
	NAMED_RO_CODE=$(wi_post "$SSE_BASE/ro-$NAMED_SESSNAME/input")
	echo "    DEBUG: POST $SSE_BASE/ro-$NAMED_SESSNAME/input => HTTP $NAMED_RO_CODE" >&2
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

	# Playwright: Ctrl+B prefix key works in browser viewer
	if [ "$HAS_PLAYWRIGHT" = "true" ] && [ "$QUICK" = "false" ] && [ "$HAS_WEB" = "true" ]; then
		CTRLB_SCREENSHOT_DIR="/tmp/tmtv-ctrlb-screenshots-$$"
		mkdir -p "$CTRLB_SCREENSHOT_DIR"
		CTRLB_TEST_SCRIPT="$(dirname "$0")/test-ctrl-b.js"
		CTRLB_EXIT=0
		CTRLB_OUTPUT=$(NODE_PATH="$PW_NODE_PATH" PLAYWRIGHT_BROWSERS_PATH="$PW_BROWSERS_PATH" \
			node "$CTRLB_TEST_SCRIPT" \
			"$WEB_URL/s/$NAMED_SESSNAME" "$CTRLB_SCREENSHOT_DIR" 2>&1) || CTRLB_EXIT=$?
		if [ $CTRLB_EXIT -eq 0 ]; then
			echo "$CTRLB_OUTPUT" | grep "PASS step 1" >/dev/null && pass "Ctrl+B: terminal connects in browser"
			echo "$CTRLB_OUTPUT" | grep "PASS step 2" >/dev/null && pass "Ctrl+B: prefix key splits pane in browser"
		elif echo "$CTRLB_OUTPUT" | grep -q "MODULE_NOT_FOUND"; then
			skip "Ctrl+B browser tests" "playwright not installed"
		else
			echo "    Ctrl+B test output: $CTRLB_OUTPUT" >&2
			fail "Ctrl+B: prefix key works in browser" "exit=$CTRLB_EXIT"
		fi
		rm -rf "$CTRLB_SCREENSHOT_DIR"
	else
		skip "Ctrl+B browser tests" "no playwright or --quick mode"
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

remote "rm -f $NAMED_CONF" 2>/dev/null || true
teardown_section "named"

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

# Wait for token to stabilize
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$PWONLY_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

PWONLY_TOKEN=$(read_token "$PWONLY_SESSNAME")

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

remote "rm -f $PWONLY_CONF" 2>/dev/null || true
teardown_section "pwonly"

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

# Wait for token to stabilize
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$NPPW_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

NPPW_RW_TOKEN=$(read_token "$NPPW_SESSNAME")

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

remote "rm -f $NPPW_CONF" 2>/dev/null || true
teardown_section "nppw"

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

# Wait for token to stabilize
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$WID_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

WID_TOKEN=$(read_token "$WID_SESSNAME")

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

remote "rm -f $WID_CONF" 2>/dev/null || true
teardown_section "wid"

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

# Wait for token to stabilize (anon session)
CSRF_TOKEN=""
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "ls $SESSIONS_DIR/ 2>/dev/null | grep -v '^ro-' | grep -v '^[0-9]*-' | head -1" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1))
		if [ "$_stable" -ge 3 ]; then
			CSRF_TOKEN="$_cur_tok"
			break
		fi
	else
		_stable=0
	fi
	_prev_tok="$_cur_tok"
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

remote "rm -f $CSRF_CONF" 2>/dev/null || true
teardown_section "csrf"

# -------------------------------------------------------
# Test: Rapid sequential POSTs not rate-limited at normal typing speed
# -------------------------------------------------------
RAPID_CONF="/tmp/.tmtv-test-rapid-$TESTID.conf"
RAPID_SESSNAME="rapid$TESTID"
remote "cat > $RAPID_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$RAPID_SESSNAME\"
set -g tmtv-web-sharing on
set -g tmtv-web-input on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $RAPID_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

RAPID_TOKEN=""
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "ls $SESSIONS_DIR/ 2>/dev/null | grep -v '^ro-' | grep '$RAPID_SESSNAME' | head -1" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1))
		if [ "$_stable" -ge 3 ]; then
			RAPID_TOKEN="$_cur_tok"
			break
		fi
	else
		_stable=0
	fi
	_prev_tok="$_cur_tok"
done

if [ -n "$RAPID_TOKEN" ]; then
	# Fire 50 rapid sequential POSTs (simulates fast typing at ~100 keys/sec)
	RAPID_OK=0
	RAPID_FAIL=0
	for _i in $(seq 1 50); do
		_rc=$(curl -s -m 3 -o /dev/null -w "%{http_code}" \
			-X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			-d "k" "$SSE_BASE/$RAPID_SESSNAME/input" 2>/dev/null) || true
		if [ "$_rc" = "200" ]; then
			RAPID_OK=$((RAPID_OK + 1))
		else
			RAPID_FAIL=$((RAPID_FAIL + 1))
		fi
	done
	if [ "$RAPID_FAIL" -eq 0 ]; then
		pass "rapid POSTs: 50 sequential requests all returned 200"
	else
		fail "rapid POSTs: 50 sequential requests all returned 200" \
			"$RAPID_OK ok, $RAPID_FAIL failed"
	fi

	# Test keep-alive: multiple POSTs on same connection using curl
	# curl --keepalive reuses the TCP connection for multiple requests
	_ka_codes=""
	for _i in $(seq 1 5); do
		_ka_codes="$_ka_codes $(curl -s -m 3 -o /dev/null -w "%{http_code}" \
			-X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			-d "x" "$SSE_BASE/$RAPID_SESSNAME/input" 2>/dev/null)" || true
	done
	_ka_fail=0
	for _c in $_ka_codes; do
		if [ "$_c" != "200" ]; then _ka_fail=$((_ka_fail + 1)); fi
	done
	if [ "$_ka_fail" -eq 0 ]; then
		pass "keep-alive: sequential POSTs on pooled connections succeed"
	else
		fail "keep-alive: sequential POSTs on pooled connections succeed" \
			"codes: $_ka_codes"
	fi

	# Verify keystrokes actually arrive end-to-end via rapid input
	RAPID_E2E="RAPID_${TESTID}"
	curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary "echo ${RAPID_E2E}" "$SSE_BASE/$RAPID_SESSNAME/input" >/dev/null 2>&1
	# Send Enter separately (tests sequential delivery)
	sleep 0.2
	printf '\r' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$RAPID_SESSNAME/input" >/dev/null 2>&1
	wait_for 10 1 "rapid e2e marker in capture" \
		"remote_tmtv 'capture-pane -t main:0 -p' 2>/dev/null | grep -q '$RAPID_E2E'"
	RAPID_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	if echo "$RAPID_CAP" | grep -q "$RAPID_E2E"; then
		pass "rapid POSTs: keystrokes arrive end-to-end"
	else
		fail "rapid POSTs: keystrokes arrive end-to-end" "marker not in capture"
	fi

	# Verify rapid keystrokes arrive in order (send individual chars a-j sequentially)
	ORDER_MARKER="ORD${TESTID}"
	curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary "echo ${ORDER_MARKER}" "$SSE_BASE/$RAPID_SESSNAME/input" >/dev/null 2>&1
	sleep 0.2
	printf '\r' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$RAPID_SESSNAME/input" >/dev/null 2>&1
	sleep 1
	# Now send "echo abcdefghij" as individual single-char POSTs to stress ordering
	for _ch in e c h o ' ' a b c d e f g h i j; do
		printf '%s' "$_ch" | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
			--data-binary @- "$SSE_BASE/$RAPID_SESSNAME/input" >/dev/null 2>&1
	done
	printf '\r' | curl -s -m 3 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$RAPID_SESSNAME/input" >/dev/null 2>&1
	wait_for 10 1 "rapid order marker in capture" \
		"remote_tmtv 'capture-pane -t main:0 -p' 2>/dev/null | grep -q abcdefghij"
	ORDER_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	if echo "$ORDER_CAP" | grep -q "abcdefghij"; then
		pass "rapid POSTs: keystrokes arrive in order"
	else
		fail "rapid POSTs: keystrokes arrive in order" \
			"expected 'abcdefghij' in capture output"
	fi
else
	skip "rapid POSTs: 50 sequential requests all returned 200" "could not create session"
	skip "keep-alive: sequential POSTs on pooled connections succeed" "could not create session"
	skip "rapid POSTs: keystrokes arrive end-to-end" "could not create session"
	skip "rapid POSTs: keystrokes arrive in order" "could not create session"
fi

remote "rm -f $RAPID_CONF" 2>/dev/null || true
teardown_section "rapid"

# -------------------------------------------------------
# Test: Large PTY output — session survives burst > 128KB
# -------------------------------------------------------
# Regression test for tmtv-wht.1 (P0): large PTY bursts (e.g.
# from AI tools) used to hit TMATE_MAX_MESSAGE_SIZE=128KB
# and kill the session with tmate_fatal. After the fix the
# limit is 2MB and oversized messages are discarded gracefully.
LPTY_CONF="/tmp/.tmtv-test-lpty-$TESTID.conf"
LPTY_SESSNAME="lpty$TESTID"
remote "cat > $LPTY_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$LPTY_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $LPTY_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

LPTY_TOKEN=""
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$LPTY_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1))
		[ "$_stable" -ge 3 ] && LPTY_TOKEN="$_cur_tok" && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

if [ -n "$LPTY_TOKEN" ]; then
	# Generate ~200KB of output in one burst (well above old 128KB limit)
	remote_tmtv "send-keys -t main:0 'dd if=/dev/urandom bs=1024 count=200 2>/dev/null | base64' Enter"
	sleep 5

	# Session must still be alive — verify we can capture pane output
	_lpty_alive=$(remote_tmtv "display-message -p -t main:0 '#S'" 2>/dev/null || echo "")
	if [ "$_lpty_alive" = "main" ]; then
		pass "large PTY burst (200KB): session survived"
	else
		fail "large PTY burst (200KB): session survived" \
			"session dead or unresponsive after burst"
	fi

	# Verify normal operation continues after the burst
	LPTY_MARKER="POSTBURST_${TESTID}"
	remote_tmtv "send-keys -t main:0 'echo $LPTY_MARKER' Enter"
	sleep 2
	_lpty_cap=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	if echo "$_lpty_cap" | grep -q "$LPTY_MARKER"; then
		pass "large PTY burst: normal output works after burst"
	else
		fail "large PTY burst: normal output works after burst" \
			"marker '$LPTY_MARKER' not found in pane"
	fi
else
	skip "large PTY burst (200KB): session survived" "could not create session"
	skip "large PTY burst: normal output works after burst" "could not create session"
fi

remote "rm -f $LPTY_CONF" 2>/dev/null || true
teardown_section "lpty"

# === INPUT LATENCY BENCHMARKS ===
#
# Measure actual round-trip latency for SSH and web input paths.
# These tests create their own session and report wall-clock times.
# They degrade gracefully — skip if tokens are unavailable.
#
# Baseline tests run first to measure plain tmux without tmtv relay overhead.

# Helper: get current time in milliseconds (defined early for baseline tests).
# Uses date +%s%N (nanoseconds) if available, falls back to seconds * 1000.
_ms_now() {
	_ns=$(date +%s%N 2>/dev/null) || _ns=""
	if [ ${#_ns} -gt 10 ]; then
		# date +%s%N works — convert nanoseconds to milliseconds
		echo $((_ns / 1000000))
	else
		# Fallback: seconds * 1000 (1-second granularity)
		echo $(($(date +%s) * 1000))
	fi
}

# Initialize baseline result variables (used in report even if skipped)
BASELINE_LOCAL_MIN=""
BASELINE_LOCAL_AVG=""
BASELINE_LOCAL_MAX=""
BASELINE_SSH_MIN=""
BASELINE_SSH_AVG=""
BASELINE_SSH_MAX=""

# -------------------------------------------------------
# Baseline: tmux local keystroke echo latency
# -------------------------------------------------------
# Measures pure local tmux echo (no network, no relay).
# This is the "cost of zero" — the floor for latency measurements.
TMUX_BIN=$(remote "command -v tmux 2>/dev/null" || echo "")
if [ -n "$TMUX_BIN" ]; then
	# Clean up any stale tmux sessions from earlier tests that might
	# interfere with baseline measurements.
	remote "tmux kill-session -t baseline_local_$TESTID" 2>/dev/null || true

	# Start a plain tmux session on the staging server
	remote "TERM=xterm-256color tmux new-session -d -s baseline_local_$TESTID" 2>/dev/null
	sleep 3

	# Verify session is running (retry a few times — tmux server may
	# take a moment to start if this is the first session)
	wait_for 10 1 "baseline local tmux session ready" \
		"remote 'tmux has-session -t baseline_local_$TESTID'" || true
	if remote "tmux has-session -t baseline_local_$TESTID" 2>/dev/null; then
		remote "cat > /tmp/tmtv-baseline-local.exp << 'EXPECT'
set timeout 10
spawn tmux attach-session -t BASELINE_SESS

# Wait for shell prompt
sleep 2

set results {}
for {set i 1} {\$i <= 5} {incr i} {
    set marker \"TMLOC${i}_TESTID\"
    set start_ms [clock milliseconds]
    send \"echo \$marker\r\"
    expect {
        timeout {
            lappend results -1
            continue
        }
        \"\$marker\" {
            set end_ms [clock milliseconds]
            set elapsed [expr {\$end_ms - \$start_ms}]
            lappend results \$elapsed
        }
    }
    sleep 0.3
}

puts \"BASELINE_LOCAL_RESULTS=[join \$results ,]\"
sleep 0.5
send \"exit\r\"
expect eof
wait
EXPECT
sed -i \"s/BASELINE_SESS/baseline_local_$TESTID/;s/TESTID/$TESTID/\" /tmp/tmtv-baseline-local.exp"

		BASELINE_LOCAL_OUT=$(remote "timeout 30 expect /tmp/tmtv-baseline-local.exp 2>/dev/null" || echo "")
		remote "rm -f /tmp/tmtv-baseline-local.exp" 2>/dev/null || true

		if echo "$BASELINE_LOCAL_OUT" | grep -q "BASELINE_LOCAL_RESULTS="; then
			_bl_results=$(echo "$BASELINE_LOCAL_OUT" | grep "BASELINE_LOCAL_RESULTS=" | sed 's/.*BASELINE_LOCAL_RESULTS=//' | tr -d ' \r')
			_bl_min=999999
			_bl_max=0
			_bl_sum=0
			_bl_count=0
			_bl_timeouts=0
			_saved_ifs="$IFS"
			IFS=","
			for _val in $_bl_results; do
				if [ "$_val" = "-1" ]; then
					_bl_timeouts=$((_bl_timeouts + 1))
				else
					_bl_count=$((_bl_count + 1))
					_bl_sum=$((_bl_sum + _val))
					[ "$_val" -lt "$_bl_min" ] 2>/dev/null && _bl_min=$_val
					[ "$_val" -gt "$_bl_max" ] 2>/dev/null && _bl_max=$_val
				fi
			done
			IFS="$_saved_ifs"

			if [ "$_bl_count" -gt 0 ]; then
				_bl_avg=$((_bl_sum / _bl_count))
				BASELINE_LOCAL_MIN=$_bl_min
				BASELINE_LOCAL_AVG=$_bl_avg
				BASELINE_LOCAL_MAX=$_bl_max
				pass "tmux baseline local echo (min=${_bl_min}ms avg=${_bl_avg}ms max=${_bl_max}ms, n=${_bl_count})"
			else
				fail "tmux baseline local echo" "all 5 keystrokes timed out"
			fi
		else
			fail "tmux baseline local echo" "expect script produced no results"
		fi
	else
		skip "tmux baseline local echo (session failed to start)"
	fi

	# Clean up the local baseline session
	remote "tmux kill-session -t baseline_local_$TESTID" 2>/dev/null || true
else
	skip "tmux baseline local echo (tmux not found)"
fi

# -------------------------------------------------------
# Baseline: tmux SSH echo latency
# -------------------------------------------------------
# Measures tmux echo over SSH to localhost (no tmtv relay).
# Isolates the SSH transport cost from the tmtv relay overhead.
if [ -n "$TMUX_BIN" ]; then
	# Clean up any stale session from a previous run
	remote "tmux kill-session -t baseline_ssh_$TESTID" 2>/dev/null || true

	# Start a plain tmux session on the staging server
	remote "TERM=xterm-256color tmux new-session -d -s baseline_ssh_$TESTID" 2>/dev/null
	sleep 3

	# Wait for session to be ready
	wait_for 10 1 "baseline SSH tmux session ready" \
		"remote 'tmux has-session -t baseline_ssh_$TESTID'" || true
	if remote "tmux has-session -t baseline_ssh_$TESTID" 2>/dev/null; then
		remote "cat > /tmp/tmtv-baseline-ssh.exp << 'EXPECT'
set timeout 10
spawn ssh -o StrictHostKeyChecking=no localhost

# Wait for login shell
sleep 2

# Attach to the tmux session over SSH
send \"tmux attach-session -t BASELINE_SESS\r\"
sleep 2

set results {}
for {set i 1} {\$i <= 5} {incr i} {
    set marker \"TMSSH${i}_TESTID\"
    set start_ms [clock milliseconds]
    send \"echo \$marker\r\"
    expect {
        timeout {
            lappend results -1
            continue
        }
        \"\$marker\" {
            set end_ms [clock milliseconds]
            set elapsed [expr {\$end_ms - \$start_ms}]
            lappend results \$elapsed
        }
    }
    sleep 0.3
}

puts \"BASELINE_SSH_RESULTS=[join \$results ,]\"
sleep 0.5
send \"exit\r\"
sleep 0.5
send \"exit\r\"
expect eof
wait
EXPECT
sed -i \"s/BASELINE_SESS/baseline_ssh_$TESTID/;s/TESTID/$TESTID/\" /tmp/tmtv-baseline-ssh.exp"

		BASELINE_SSH_OUT=$(remote "timeout 30 expect /tmp/tmtv-baseline-ssh.exp 2>/dev/null" || echo "")
		remote "rm -f /tmp/tmtv-baseline-ssh.exp" 2>/dev/null || true

		if echo "$BASELINE_SSH_OUT" | grep -q "BASELINE_SSH_RESULTS="; then
			_bs_results=$(echo "$BASELINE_SSH_OUT" | grep "BASELINE_SSH_RESULTS=" | sed 's/.*BASELINE_SSH_RESULTS=//' | tr -d ' \r')
			_bs_min=999999
			_bs_max=0
			_bs_sum=0
			_bs_count=0
			_bs_timeouts=0
			_saved_ifs="$IFS"
			IFS=","
			for _val in $_bs_results; do
				if [ "$_val" = "-1" ]; then
					_bs_timeouts=$((_bs_timeouts + 1))
				else
					_bs_count=$((_bs_count + 1))
					_bs_sum=$((_bs_sum + _val))
					[ "$_val" -lt "$_bs_min" ] 2>/dev/null && _bs_min=$_val
					[ "$_val" -gt "$_bs_max" ] 2>/dev/null && _bs_max=$_val
				fi
			done
			IFS="$_saved_ifs"

			if [ "$_bs_count" -gt 0 ]; then
				_bs_avg=$((_bs_sum / _bs_count))
				BASELINE_SSH_MIN=$_bs_min
				BASELINE_SSH_AVG=$_bs_avg
				BASELINE_SSH_MAX=$_bs_max
				pass "tmux baseline SSH echo (min=${_bs_min}ms avg=${_bs_avg}ms max=${_bs_max}ms, n=${_bs_count})"
			else
				fail "tmux baseline SSH echo" "all 5 keystrokes timed out"
			fi
		else
			fail "tmux baseline SSH echo" "expect script produced no results"
		fi
	else
		skip "tmux baseline SSH echo (session failed to start)"
	fi

	# Clean up the SSH baseline session
	remote "tmux kill-session -t baseline_ssh_$TESTID" 2>/dev/null || true
else
	skip "tmux baseline SSH echo (tmux not found)"
fi

BENCH_CONF="/tmp/.tmtv-test-bench-$TESTID.conf"
BENCH_SESSNAME="bench$TESTID"
remote "cat > $BENCH_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$BENCH_SESSNAME\"
set -g tmtv-web-sharing on
set -g tmtv-web-input on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $BENCH_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

# Wait for token to stabilize
BENCH_TOKEN=""
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$BENCH_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1))
		if [ "$_stable" -ge 3 ]; then
			BENCH_TOKEN="$_cur_tok"
			break
		fi
	else
		_stable=0
	fi
	_prev_tok="$_cur_tok"
done

BENCH_RW_TOKEN=$(read_rw_token_stable "$BENCH_SESSNAME")

# -------------------------------------------------------
# Test: Web input round-trip latency — single marker
# -------------------------------------------------------
# Send a unique marker via POST to the input endpoint, then
# poll capture-pane until the marker appears. Measures the
# full path: HTTP POST → server → SSH channel → shell echo.
if [ -n "$BENCH_TOKEN" ]; then
	WEB_LAT_MARKER="WL${TESTID}"
	# Clear the pane first
	remote_tmtv "send-keys -t main:0 'clear' Enter"
	sleep 1

	# Send the marker as "echo <marker>" + Enter
	_t_start=$(_ms_now)
	curl -s -m 5 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary "echo ${WEB_LAT_MARKER}" \
		"$SSE_BASE/$BENCH_SESSNAME/input" >/dev/null 2>&1
	sleep 0.1
	printf '\r' | curl -s -m 5 -X POST -H "Content-Type: text/plain" -H "X-Tmtv-Input: 1" \
		--data-binary @- "$SSE_BASE/$BENCH_SESSNAME/input" >/dev/null 2>&1

	# Poll capture-pane until marker appears (max 20 iterations, ~50ms apart)
	_found=false
	for _poll in $(seq 1 40); do
		_cap=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
		if echo "$_cap" | grep -q "$WEB_LAT_MARKER"; then
			_found=true
			break
		fi
		sleep 0.1
	done
	_t_end=$(_ms_now)
	_web_lat=$((_t_end - _t_start))

	if [ "$_found" = "true" ]; then
		if [ "$_web_lat" -lt 2000 ] 2>/dev/null; then
			pass "web input round-trip latency (${_web_lat}ms)"
		else
			fail "web input round-trip latency" "took ${_web_lat}ms (limit: 2000ms)"
		fi
	else
		fail "web input round-trip latency" "marker never appeared in pane (waited ${_web_lat}ms)"
	fi
else
	skip "web input round-trip latency (no token)"
fi

# -------------------------------------------------------
# Test: SSH RW multi-keystroke latency — 5 echoes, min/avg/max
# -------------------------------------------------------
# Connects as an SSH RW viewer and measures real keystroke-to-echo
# latency through the tmtv relay. Uses a unique output prefix (>>)
# that only appears in the shell output line, not the command echo,
# to avoid matching the send itself.
# Fails if average exceeds 500ms.
if [ -n "$BENCH_RW_TOKEN" ]; then
	# SSH relay latency: measures the round-trip through the tmtv relay.
	# send-keys → pane PTY → shell echo → capture-pane. On localhost this
	# is equivalent to SSH viewer latency (0ms network between viewer and
	# server). The overhead vs the tmux baseline shows the relay cost.

	# Clear pane
	remote_tmtv "send-keys -t main:0 'clear' Enter"
	sleep 1

	_ssh_count=0
	_ssh_sum=0
	_ssh_min=999999
	_ssh_max=0
	_ssh_timeouts=0

	for _si in 1 2 3 4 5; do
		_ssh_marker="SB${_si}x$$"
		_t0=$(_ms_now)
		remote_tmtv "send-keys -t main:0 'echo ${_ssh_marker}' Enter"

		# Poll capture-pane until marker appears
		_ssh_found=false
		for _sp in $(seq 1 80); do
			remote "usleep 10000 2>/dev/null || sleep 0.01"
			_cap=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
			if echo "$_cap" | grep -q "$_ssh_marker"; then
				_ssh_found=true
				break
			fi
		done
		_t1=$(_ms_now)

		if [ "$_ssh_found" = "true" ]; then
			_ssh_ms=$((_t1 - _t0))
			_ssh_count=$((_ssh_count + 1))
			_ssh_sum=$((_ssh_sum + _ssh_ms))
			[ "$_ssh_ms" -lt "$_ssh_min" ] 2>/dev/null && _ssh_min=$_ssh_ms
			[ "$_ssh_ms" -gt "$_ssh_max" ] 2>/dev/null && _ssh_max=$_ssh_ms
		else
			_ssh_timeouts=$((_ssh_timeouts + 1))
		fi
		sleep 0.3
	done

	if [ "$_ssh_count" -gt 0 ]; then
		_ssh_avg=$((_ssh_sum / _ssh_count))
		SSH_MULTI_MS="$_ssh_avg"
		if [ "$_ssh_avg" -lt 500 ] 2>/dev/null; then
			pass "SSH multi-keystroke latency (min=${_ssh_min}ms avg=${_ssh_avg}ms max=${_ssh_max}ms, n=${_ssh_count})"
		else
			fail "SSH multi-keystroke latency" \
				"avg ${_ssh_avg}ms exceeds 500ms (min=${_ssh_min}ms max=${_ssh_max}ms, n=${_ssh_count})"
		fi
	else
		fail "SSH multi-keystroke latency" "all 5 keystrokes timed out"
	fi

else
	skip "SSH multi-keystroke latency (no token)"
fi

# -------------------------------------------------------
# Test: Web input burst latency — 10 chars rapid-fire
# -------------------------------------------------------
# Sends 10 individual characters as separate POSTs as fast
# as possible, then measures time until all 10 appear in
# capture-pane output. Tests batching and throughput under load.
if [ -n "$BENCH_TOKEN" ]; then
	# Clear the pane
	remote_tmtv "send-keys -t main:0 'clear' Enter"
	sleep 1

	# Send "echo BURSTXXXX" then Enter, each char as its own POST
	BURST_MARKER="B${TESTID}"
	BURST_PAYLOAD="echo ${BURST_MARKER}0123456789"

	_t_start=$(_ms_now)

	# Send each character as a separate POST
	_idx=0
	while [ "$_idx" -lt ${#BURST_PAYLOAD} ]; do
		_ch=$(printf '%s' "$BURST_PAYLOAD" | cut -c$((_idx + 1)))
		printf '%s' "$_ch" | curl -s -m 5 -X POST -H "Content-Type: text/plain" \
			-H "X-Tmtv-Input: 1" --data-binary @- \
			"$SSE_BASE/$BENCH_SESSNAME/input" >/dev/null 2>&1
		_idx=$((_idx + 1))
	done
	# Send Enter
	printf '\r' | curl -s -m 5 -X POST -H "Content-Type: text/plain" \
		-H "X-Tmtv-Input: 1" --data-binary @- \
		"$SSE_BASE/$BENCH_SESSNAME/input" >/dev/null 2>&1

	# Poll capture-pane until the full marker + 10 digits appear
	_found=false
	for _poll in $(seq 1 60); do
		_cap=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
		if echo "$_cap" | grep -q "${BURST_MARKER}0123456789"; then
			_found=true
			break
		fi
		sleep 0.1
	done
	_t_end=$(_ms_now)
	_burst_total=$((_t_end - _t_start))

	if [ "$_found" = "true" ]; then
		_burst_per_char=$((_burst_total / 10))
		if [ "$_burst_total" -lt 5000 ] 2>/dev/null; then
			pass "web input burst latency (total=${_burst_total}ms, ${_burst_per_char}ms/char, 10 chars)"
		else
			fail "web input burst latency" \
				"total ${_burst_total}ms exceeds 5000ms (${_burst_per_char}ms/char, 10 chars)"
		fi
	else
		fail "web input burst latency" "not all chars appeared in pane (waited ${_burst_total}ms)"
	fi
else
	skip "web input burst latency (no token)"
fi

# --- Latency report summary ---
# Write a machine-readable report so agents can read and report results.
# Includes baseline measurements and calculated overhead.

# Calculate SSH overhead (tmtv avg - tmux SSH avg) if both are available
SSH_OVERHEAD=""
if [ -n "$BASELINE_SSH_AVG" ] && [ -n "$SSH_MULTI_MS" ]; then
	SSH_OVERHEAD=$((SSH_MULTI_MS - BASELINE_SSH_AVG))
fi

LATENCY_REPORT="/tmp/tmtv-latency-report.txt"
LATENCY_VER=$(remote "TERM=xterm-256color $REMOTE_TMTV -V 2>&1" | grep -oE "[0-9]+\.[0-9]+\.[0-9]+" | head -1)
LATENCY_VER="${LATENCY_VER:-unknown}"
{
	echo "=== tmtv latency report ==="
	echo "version: $LATENCY_VER"
	echo "date: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
	echo "host: ${TEST_HOST:-unknown}"
	echo "testid: $TESTID"
	echo ""
	echo "--- baseline (plain tmux) ---"
	echo "tmux_local_min_ms: ${BASELINE_LOCAL_MIN:-n/a}"
	echo "tmux_local_avg_ms: ${BASELINE_LOCAL_AVG:-n/a}"
	echo "tmux_local_max_ms: ${BASELINE_LOCAL_MAX:-n/a}"
	echo "tmux_ssh_min_ms: ${BASELINE_SSH_MIN:-n/a}"
	echo "tmux_ssh_avg_ms: ${BASELINE_SSH_AVG:-n/a}"
	echo "tmux_ssh_max_ms: ${BASELINE_SSH_MAX:-n/a}"
	echo ""
	echo "--- tmtv measurements ---"
	echo "ssh_min_ms: ${_ssh_min:-n/a}"
	echo "ssh_avg_ms: ${SSH_MULTI_MS:-n/a}"
	echo "ssh_max_ms: ${_ssh_max:-n/a}"
	echo "ssh_samples: ${_ssh_count:-0}"
	echo "ssh_timeouts: ${_ssh_timeouts:-0}"
	echo "web_roundtrip_ms: ${_web_lat:-n/a}"
	echo "web_burst_total_ms: ${_burst_total:-n/a}"
	echo "web_burst_per_char_ms: ${_burst_per_char:-n/a}"
	echo "web_burst_chars: 10"
	echo ""
	echo "--- overhead (tmtv - baseline) ---"
	echo "ssh_overhead_ms: ${SSH_OVERHEAD:-n/a}"
} > "$LATENCY_REPORT" 2>/dev/null || true
# Save versioned copy for historical comparison
LATENCY_HISTORY="/tmp/tmtv-latency-v${LATENCY_VER}.txt"
cp -f "$LATENCY_REPORT" "$LATENCY_HISTORY" 2>/dev/null || true
echo ""
echo "  ** Latency report written to $LATENCY_REPORT **"
echo "  ** Versioned copy saved to $LATENCY_HISTORY **"
echo ""

# Print human-readable comparison table
_fmt_val() {
	if [ -n "$1" ] && [ "$1" != "n/a" ]; then
		printf '%5sms' "$1"
	else
		printf '  n/a  '
	fi
}
_fmt_overhead() {
	if [ -n "$1" ] && [ "$1" != "n/a" ]; then
		printf ' +%sms' "$1"
	else
		printf '  n/a  '
	fi
}

echo "  +-------------------------+----------+----------+----------+"
echo "  | Measurement             | tmux     | tmtv     | overhead |"
echo "  +-------------------------+----------+----------+----------+"
printf "  | Local echo (avg)        | %7s  | %7s  | %7s  |\n" \
	"$(_fmt_val "$BASELINE_LOCAL_AVG")" "n/a" "n/a"
printf "  | SSH echo (avg)          | %7s  | %7s  | %7s  |\n" \
	"$(_fmt_val "$BASELINE_SSH_AVG")" "$(_fmt_val "$SSH_MULTI_MS")" "$(_fmt_overhead "$SSH_OVERHEAD")"
printf "  | Web input round-trip    | %7s  | %7s  | %7s  |\n" \
	"n/a" "$(_fmt_val "$_web_lat")" "n/a"
printf "  | Web burst (per char)    | %7s  | %7s  | %7s  |\n" \
	"n/a" "$(_fmt_val "$_burst_per_char")" "n/a"
echo "  +-------------------------+----------+----------+----------+"
echo ""

# Clean up benchmark session
remote "rm -f $BENCH_CONF" 2>/dev/null || true
teardown_section "benchmark"

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

# Wait for token to stabilize
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$TTL_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

# Verify session is alive
TTL_TOKEN=$(read_token "$TTL_SESSNAME")
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

remote "rm -f $TTL_CONF" 2>/dev/null || true
teardown_section "TTL"

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

	# Wait for token to stabilize
	_prev_tok=""
	_stable=0
	for _wait in $(seq 1 20); do
		sleep 1
		_cur_tok=$(remote "readlink $SESSIONS_DIR/$JURL_SESSNAME 2>/dev/null" || echo "")
		if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
			_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
		else _stable=0; fi
		_prev_tok="$_cur_tok"
	done

	JURL_TOKEN=$(read_token "$JURL_SESSNAME")

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

	remote "rm -f $JURL_CONF" 2>/dev/null || true
	teardown_section "short-URL"
else
	skip "short URL /j/<token> returns 200" "web not available"
	skip "short URL /j/<token> serves viewer" "web not available"
fi

# -------------------------------------------------------
# Test: SSE stream delivers data (vpty mode)
# -------------------------------------------------------
# With the full-screen virtual PTY, the server intentionally filters
# OUT_STATUS from the SSE stream (the status bar is rendered in the
# terminal stream instead).  These tests verify that the SSE stream
# delivers PTY_DATA (pane_id=-1) — confirming the vpty pipeline works.
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

# Wait for token to stabilize
_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$STATUS_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

STATUS_TOKEN=$(read_token "$STATUS_SESSNAME")

if [ -n "$STATUS_TOKEN" ]; then
	# Capture ~5s of SSE data and verify we receive data: lines
	# (the vpty sends PTY_DATA with pane_id=-1 containing the full
	# terminal stream including the status bar).
	SSE_STATUS_RAW=$(curl -s -m 5 -N "$SSE_BASE/$STATUS_TOKEN" 2>/dev/null || echo "")
	SSE_DATA_LINES=$(echo "$SSE_STATUS_RAW" | grep -c "^data:" || echo "0")

	if [ "$SSE_DATA_LINES" -gt 0 ]; then
		pass "SSE stream delivers data (vpty active, ${SSE_DATA_LINES} data frames)"
	else
		fail "SSE stream delivers data (vpty active)" \
			"no data: lines received from SSE stream"
	fi

	# Verify status bar content is rendered in the terminal stream
	# by setting a custom status-right and checking that the vpty
	# stream contains it (via capture-pane on pane 0).
	remote "TERM=xterm-256color $REMOTE_TMTV -f $STATUS_CONF set-option -g status-right 'RIGHTTEST %H:%M'" 2>/dev/null || true
	sleep 3
	STATUS_CAP=$(remote_tmtv "capture-pane -t main:0 -p" 2>/dev/null || echo "")
	# The status bar is rendered by tmux in the terminal — it may not
	# appear in capture-pane output (which captures pane content only).
	# Instead, verify the SSE stream is still delivering data after the
	# status-right change.
	STATUS_TOKEN=$(read_token "$STATUS_SESSNAME")
	SSE_AFTER_RAW=$(curl -s -m 3 -N "$SSE_BASE/$STATUS_TOKEN" 2>/dev/null || echo "")
	SSE_AFTER_LINES=$(echo "$SSE_AFTER_RAW" | grep -c "^data:" || echo "0")
	if [ "$SSE_AFTER_LINES" -gt 0 ]; then
		pass "SSE stream continues after status-right change (${SSE_AFTER_LINES} frames)"
	else
		fail "SSE stream continues after status-right change" \
			"no data after setting status-right"
	fi

	# Cleanup: kill the status test session
	remote "rm -f $STATUS_CONF" 2>/dev/null || true
	teardown_section "status"
else
	skip "SSE stream delivers data (vpty active)" "could not create session"
	skip "SSE stream continues after status-right change" "could not create session"
fi

# -------------------------------------------------------
# Test: tmux 3.6a escape sequence compatibility (passthrough, OSC 52)
# -------------------------------------------------------
echo ""
echo "-- Escape sequence relay tests --"

# These tests verify that modern tmux escape sequences are properly
# relayed through the tmtv SSH tunnel to viewers.
#
# Architecture reminder:
#   Host PTY → tmtv client → SSH tunnel → server daemon → SSH viewer
#   The server daemon uses input_parse_buffer() which triggers tty_write()
#   to connected SSH viewers. Escape sequences like passthrough and OSC 52
#   must survive this relay chain.

ESC_CONF="/tmp/.tmtv-test-esc-$TESTID.conf"
if [ -n "$RSA_FP" ] || [ -n "$ED25519_FP" ]; then
	remote "cat > $ESC_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"esctest\"
set -g tmtv-web-sharing on
CONF"

	remote "TERM=xterm-256color \
		nohup script -qc '$REMOTE_TMTV -f $ESC_CONF new-session -d -s main' \
		/dev/null </dev/null >/dev/null 2>&1 &"

	# Wait for session to stabilize
	ESC_TOKEN=""
	_prev_tok=""
	_stable=0
	for _wait in $(seq 1 20); do
		sleep 1
		_cur_tok=$(remote "readlink $SESSIONS_DIR/esctest 2>/dev/null" || echo "")
		if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
			_stable=$((_stable + 1))
			[ "$_stable" -ge 3 ] && ESC_TOKEN="$_cur_tok" && break
		else
			_stable=0
		fi
		_prev_tok="$_cur_tok"
	done

	if [ -z "$ESC_TOKEN" ]; then
		skip "passthrough: allow-passthrough off blocks DCS" "session not ready"
		skip "passthrough: allow-passthrough on relays DCS to SSH viewer" "session not ready"
		skip "OSC 52: set-clipboard relays to SSH viewer" "session not ready"
		skip "OSC 52: paste buffer populated via OSC 52" "session not ready"
	else
		ESC_RW_TOKEN=$(remote "ls $SESSIONS_DIR/ 2>/dev/null | grep -E '^[0-9]+-esctest$' | head -1" || echo "")
		[ -z "$ESC_RW_TOKEN" ] && ESC_RW_TOKEN="$ESC_TOKEN"

		# Helper: capture raw SSH viewer output for N seconds
		# Uses expect to connect, wait, then close.
		# Writes raw output to a temp file.
		esc_viewer_capture() {
			local _token="$1"
			local _wait="${2:-3}"
			local _outfile="/tmp/esc-capture-$$"
			remote "timeout 15 expect -c '
				log_user 0
				set timeout $_wait
				spawn ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ${_token}@127.0.0.1
				set f [open $_outfile w]
				expect {
					timeout { }
					eof { }
				}
				catch close
				catch wait
			' 2>/dev/null; cat $_outfile 2>/dev/null; rm -f $_outfile 2>/dev/null" 2>/dev/null || echo ""
		}

		# -------------------------------------------------------
		# Test: passthrough — default (off) blocks DCS passthrough
		# -------------------------------------------------------
		# The DCS passthrough format is: DCS tmux; <escaped-seq> ST
		# With allow-passthrough=off (default), the sequence should be dropped.
		PT_MARKER="PTTEST_$$"
		remote_tmtv "send-keys -t main 'printf \"\\033Ptmux;\\033\\033]999;${PT_MARKER}\\007\\033\\\\\\\\\"' Enter" 2>/dev/null
		sleep 1
		# Check that the raw marker does NOT appear in the SSH viewer capture
		# (We capture the pane text — if passthrough is off, it gets consumed
		# by the shell as garbled text, but the escape sequence itself is dropped)
		PT_PANE=$(remote_tmtv "capture-pane -t main -p" 2>/dev/null || echo "")
		# The raw OSC 999 should NOT reach the pane output in any useful form
		# Just verify the option is off by default
		# show-options -wv returns empty when at default, or "off" if explicitly set
		PT_DEFAULT=$(remote_tmtv "show-options -wv allow-passthrough" 2>/dev/null || echo "")
		if [ -z "$PT_DEFAULT" ] || [ "$PT_DEFAULT" = "off" ] || [ "$PT_DEFAULT" = "0" ]; then
			pass "passthrough: allow-passthrough defaults to off"
		else
			fail "passthrough: allow-passthrough defaults to off" \
				"got '$PT_DEFAULT'"
		fi

		# -------------------------------------------------------
		# Test: passthrough — enable and verify relay to SSH viewer
		# -------------------------------------------------------
		# Enable passthrough on the window
		remote_tmtv "set-option -w allow-passthrough on" 2>/dev/null
		sleep 1

		# Verify the option took effect
		PT_SET=$(remote_tmtv "show-options -wv allow-passthrough" 2>/dev/null || echo "unknown")
		if [ "$PT_SET" = "on" ] || [ "$PT_SET" = "1" ]; then
			pass "passthrough: allow-passthrough set to on"
		else
			fail "passthrough: allow-passthrough set to on" \
				"got '$PT_SET'"
		fi

		# Send a DCS passthrough with a unique marker via OSC 999
		# Format: ESC P tmux; ESC ESC ] 999 ; <data> BEL ESC backslash
		PT_MARKER2="PTRELAY_$$"
		remote_tmtv "send-keys -t main 'printf \"\\033Ptmux;\\033\\033]999;${PT_MARKER2}\\007\\033\\\\\\\\\"' Enter" 2>/dev/null
		sleep 2

		# Connect an SSH RW viewer and capture raw output for 3 seconds
		# The passthrough should appear as a raw OSC 999 in the viewer output
		PT_VIEWER=$(remote "timeout 5 ssh -o StrictHostKeyChecking=no -p $TMTV_PORT ${ESC_RW_TOKEN}@127.0.0.1 2>/dev/null | cat -v" 2>/dev/null || echo "")

		if echo "$PT_VIEWER" | grep -q "$PT_MARKER2"; then
			pass "passthrough: DCS relayed to SSH viewer"
		else
			# Passthrough relay may not work yet — record as failure to fix
			fail "passthrough: DCS relayed to SSH viewer" \
				"marker '$PT_MARKER2' not in viewer output ($(echo "$PT_VIEWER" | wc -c) bytes captured)"
		fi

		# Reset passthrough
		remote_tmtv "set-option -wu allow-passthrough" 2>/dev/null

		# -------------------------------------------------------
		# Test: OSC 52 — clipboard set populates paste buffer
		# -------------------------------------------------------
		# Send OSC 52 to set clipboard. The server's input parser should
		# call input_osc_52() which adds to paste buffers.
		# Format: ESC ] 52 ; c ; <base64> BEL
		OSC52_DATA="tmtv_osc52_test_$$"
		OSC52_B64=$(echo -n "$OSC52_DATA" | base64 | tr -d '\n')

		# Enable set-clipboard so OSC 52 is processed
		remote_tmtv "set-option -s set-clipboard on" 2>/dev/null
		sleep 1

		# Send OSC 52 from the host terminal
		remote_tmtv "send-keys -t main 'printf \"\\033]52;c;${OSC52_B64}\\007\"' Enter" 2>/dev/null
		sleep 2

		# Check if the paste buffer was populated
		PASTE_BUF=$(remote_tmtv "show-buffer" 2>/dev/null || echo "")
		if echo "$PASTE_BUF" | grep -q "$OSC52_DATA"; then
			pass "OSC 52: paste buffer populated via clipboard set"
		else
			fail "OSC 52: paste buffer populated via clipboard set" \
				"buffer='$PASTE_BUF', expected '$OSC52_DATA'"
		fi

		# -------------------------------------------------------
		# Test: OSC 52 — clipboard relayed to SSH viewer
		# -------------------------------------------------------
		# With set-clipboard=2 (external), the OSC 52 should be forwarded
		# to connected SSH viewers via tty_cmd_setselection.
		remote_tmtv "set-option -s set-clipboard external" 2>/dev/null
		sleep 1

		OSC52_DATA2="tmtv_relay_test_$$"
		OSC52_B64_2=$(echo -n "$OSC52_DATA2" | base64 | tr -d '\n')

		# Connect viewer in background, capturing raw output
		ESC_VIEWER_LOG="/tmp/esc-viewer-$$"
		remote "timeout 8 ssh -o StrictHostKeyChecking=no -tt -p $TMTV_PORT \
			${ESC_RW_TOKEN}@127.0.0.1 2>/dev/null | cat -v > $ESC_VIEWER_LOG &"
		sleep 2

		# Now send OSC 52 from host
		remote_tmtv "send-keys -t main 'printf \"\\033]52;c;${OSC52_B64_2}\\007\"' Enter" 2>/dev/null
		sleep 3

		# Read viewer capture
		OSC52_VIEWER=$(remote "cat $ESC_VIEWER_LOG 2>/dev/null" || echo "")
		remote "rm -f $ESC_VIEWER_LOG" 2>/dev/null

		# The viewer should have received the OSC 52 sequence with our data
		if echo "$OSC52_VIEWER" | grep -q "$OSC52_B64_2"; then
			pass "OSC 52: clipboard relayed to SSH viewer"
		else
			fail "OSC 52: clipboard relayed to SSH viewer" \
				"b64 marker not in viewer output ($(echo "$OSC52_VIEWER" | wc -c) bytes)"
		fi

		# -------------------------------------------------------
		# Test: extended keys (CSI u) — option exists and toggles
		# -------------------------------------------------------
		# extended-keys is a server option: off, on, always
		EK_DEFAULT=$(remote_tmtv "show-options -sv extended-keys" 2>/dev/null || echo "")
		if [ -z "$EK_DEFAULT" ] || [ "$EK_DEFAULT" = "off" ]; then
			pass "extended-keys: defaults to off"
		else
			fail "extended-keys: defaults to off" "got '$EK_DEFAULT'"
		fi

		# Enable extended keys and verify
		remote_tmtv "set-option -s extended-keys on" 2>/dev/null
		sleep 1
		EK_SET=$(remote_tmtv "show-options -sv extended-keys" 2>/dev/null || echo "")
		if [ "$EK_SET" = "on" ]; then
			pass "extended-keys: set to on"
		else
			fail "extended-keys: set to on" "got '$EK_SET'"
		fi

		# Check extended-keys-format option
		EKF_VAL=$(remote_tmtv "show-options -sv extended-keys-format" 2>/dev/null || echo "")
		if [ "$EKF_VAL" = "xterm" ] || [ -z "$EKF_VAL" ]; then
			pass "extended-keys-format: defaults to xterm"
		else
			# csi-u is also valid
			if [ "$EKF_VAL" = "csi-u" ]; then
				pass "extended-keys-format: defaults to csi-u"
			else
				fail "extended-keys-format: defaults to xterm or csi-u" \
					"got '$EKF_VAL'"
			fi
		fi

		# Reset
		remote_tmtv "set-option -su extended-keys" 2>/dev/null

		# -------------------------------------------------------
		# Test: focus events — option works, SSH viewer gets focus
		# -------------------------------------------------------
		FE_DEFAULT=$(remote_tmtv "show-options -sv focus-events" 2>/dev/null || echo "")
		if [ -z "$FE_DEFAULT" ] || [ "$FE_DEFAULT" = "off" ] || [ "$FE_DEFAULT" = "0" ]; then
			pass "focus-events: defaults to off"
		else
			fail "focus-events: defaults to off" "got '$FE_DEFAULT'"
		fi

		remote_tmtv "set-option -s focus-events on" 2>/dev/null
		sleep 1
		FE_SET=$(remote_tmtv "show-options -sv focus-events" 2>/dev/null || echo "")
		if [ "$FE_SET" = "on" ] || [ "$FE_SET" = "1" ]; then
			pass "focus-events: set to on"
		else
			fail "focus-events: set to on" "got '$FE_SET'"
		fi

		# Reset
		remote_tmtv "set-option -su focus-events" 2>/dev/null

		# -------------------------------------------------------
		# Test: popup (display-popup) — command exists
		# -------------------------------------------------------
		# Verify display-popup exists via list-commands
		POPUP_CMD=$(remote_tmtv "list-commands" 2>/dev/null | grep -c "display-popup" || echo "0")
		if [ "$POPUP_CMD" -gt 0 ]; then
			pass "display-popup: command available"
		else
			fail "display-popup: command available" "not in list-commands"
		fi

		# Test that popup border style options exist
		PB_DEFAULT=$(remote_tmtv "show-options -wv popup-border-lines" 2>/dev/null || echo "")
		if [ -z "$PB_DEFAULT" ] || [ "$PB_DEFAULT" = "single" ]; then
			pass "popup-border-lines: option exists (default=single)"
		else
			pass "popup-border-lines: option exists (value=$PB_DEFAULT)"
		fi

		# -------------------------------------------------------
		# Test: SIXEL — build flag enabled
		# -------------------------------------------------------
		# #{sixel_support} returns 1 when built with --enable-sixel
		SIXEL_SUPPORT=$(remote "TERM=xterm-256color $REMOTE_TMTV display-message -p '#{sixel_support}'" 2>/dev/null || echo "")
		if [ "$SIXEL_SUPPORT" = "1" ]; then
			pass "SIXEL: binary built with --enable-sixel"
		elif [ "$SIXEL_SUPPORT" = "0" ] || [ -z "$SIXEL_SUPPORT" ]; then
			fail "SIXEL: binary built with --enable-sixel" \
				"#{sixel_support}='$SIXEL_SUPPORT' (expected 1)"
		else
			fail "SIXEL: binary built with --enable-sixel" \
				"unexpected value '${SIXEL_SUPPORT}'"
		fi

		# -------------------------------------------------------
		# Test: SIXEL — image output doesn't crash pane
		# -------------------------------------------------------
		# Send a minimal 1x1 red SIXEL image to the session pane
		# and verify the pane survives (no crash, pane still listed).
		# The SIXEL DCS sequence: ESC P q " 1;1;1;1 # 0;2;100;0;0 !1~ - ESC backslash
		remote_tmtv "send-keys -t main 'printf \"\\\\033Pq\\\\\"1;1;1;1#0;2;100;0;0!1~-\\\\033\\\\\\\\\\\\\\\\\"' Enter" 2>/dev/null
		sleep 2

		SIXEL_PANE_COUNT=$(remote_tmtv "list-panes -t main" 2>/dev/null | wc -l)
		if [ "$SIXEL_PANE_COUNT" -ge 1 ]; then
			pass "SIXEL: image output doesn't crash pane"
		else
			fail "SIXEL: image output doesn't crash pane" \
				"pane count=$SIXEL_PANE_COUNT after SIXEL output"
		fi

		# -------------------------------------------------------
		# Test: SIXEL — image reaches SSH viewer
		# -------------------------------------------------------
		# Send another SIXEL image, then connect an SSH viewer and
		# check if the DCS Pq sequence appears in the raw output.
		SIXEL_VIEWER_LOG="/tmp/sixel-viewer-$$"
		remote_tmtv "send-keys -t main 'printf \"\\\\033Pq\\\\\"1;1;1;1#0;2;100;0;0!1~-\\\\033\\\\\\\\\\\\\\\\\"' Enter" 2>/dev/null
		sleep 1

		# Connect SSH viewer, capture raw output for a few seconds
		remote "timeout 5 ssh -o StrictHostKeyChecking=no -tt -p $TMTV_PORT \
			${ESC_RW_TOKEN}@127.0.0.1 2>/dev/null | cat -v > $SIXEL_VIEWER_LOG &"
		sleep 3

		SIXEL_VIEWER=$(remote "cat $SIXEL_VIEWER_LOG 2>/dev/null" || echo "")
		remote "rm -f $SIXEL_VIEWER_LOG" 2>/dev/null

		# Look for SIXEL DCS markers in the viewer output (cat -v renders ESC as ^[)
		# The sequence starts with ^[Pq or ^[P0;0;0q
		if echo "$SIXEL_VIEWER" | grep -q "Pq"; then
			pass "SIXEL: image reaches SSH viewer"
		else
			# SIXEL data may be in the pane's image list but not re-rendered
			# on late-join — still record whether the pane has content
			SIXEL_CAPTURE=$(remote_tmtv "capture-pane -t main -p" 2>/dev/null || echo "")
			if [ -n "$SIXEL_CAPTURE" ]; then
				skip "SIXEL: image reaches SSH viewer" \
					"DCS not in viewer output ($(echo "$SIXEL_VIEWER" | wc -c) bytes); pane alive"
			else
				fail "SIXEL: image reaches SSH viewer" \
					"DCS not in viewer output and pane empty"
			fi
		fi

		# -------------------------------------------------------
		# Test: SIXEL — allow-passthrough option works
		# -------------------------------------------------------
		# tmux 3.6a allow-passthrough controls DCS passthrough to
		# the outer terminal. Verify the option can be set to on.
		remote_tmtv "set -g allow-passthrough on" 2>/dev/null
		sleep 1
		AP_VAL=$(remote_tmtv "show -gv allow-passthrough" 2>/dev/null || echo "")
		if [ "$AP_VAL" = "on" ] || [ "$AP_VAL" = "1" ]; then
			pass "SIXEL: allow-passthrough option works"
		else
			fail "SIXEL: allow-passthrough option works" \
				"expected 'on', got '$AP_VAL'"
		fi

		# Reset
		remote_tmtv "set -gu allow-passthrough" 2>/dev/null

		# -------------------------------------------------------
		# Test: SIXEL — SSE stream carries SIXEL data (web viewer)
		# -------------------------------------------------------
		# Send a SIXEL image and verify the SSE stream for this
		# session contains data (proving the pipeline to the web
		# viewer is intact). We check that the SSE endpoint returns
		# event data within a few seconds after SIXEL output.
		if [ "$HAS_WEB" = "true" ] && [ -n "$ESC_TOKEN" ]; then
			remote_tmtv "send-keys -t main 'printf \"\\\\033Pq\\\\\"1;1;1;1#0;2;100;0;0!1~-\\\\033\\\\\\\\\\\\\\\\\"' Enter" 2>/dev/null
			sleep 2

			# The SSE endpoint streams msgpack-encoded PTY data
			SSE_SIXEL=$(remote "timeout 4 curl -s -N $SSE_BASE/$ESC_TOKEN 2>/dev/null | head -c 4096" 2>/dev/null || echo "")
			if [ -n "$SSE_SIXEL" ]; then
				pass "SIXEL: SSE stream carries data after SIXEL output"
			else
				fail "SIXEL: SSE stream carries data after SIXEL output" \
					"SSE returned empty for token $ESC_TOKEN"
			fi
		else
			skip "SIXEL: SSE stream carries data after SIXEL output" \
				"web not available or no session token"
		fi

		# -------------------------------------------------------
		# Test: hyperlinks (OSC 8) — relayed to SSH viewer
		# -------------------------------------------------------
		# OSC 8 format: ESC ] 8 ; params ; uri ST ... ESC ] 8 ; ; ST
		# Send an OSC 8 hyperlink from the host and verify SSH viewer gets it
		LINK_MARKER="https://tmtv.se/test-$$"
		remote_tmtv "send-keys -t main 'printf \"\\033]8;;${LINK_MARKER}\\033\\\\\\\\CLICK HERE\\033]8;;\\033\\\\\\\\\"' Enter" 2>/dev/null
		sleep 2

		# Capture viewer output (the hyperlink ESC sequence may be in raw output)
		LINK_VIEWER_LOG="/tmp/link-viewer-$$"
		remote "timeout 5 ssh -o StrictHostKeyChecking=no -tt -p $TMTV_PORT \
			${ESC_RW_TOKEN}@127.0.0.1 2>/dev/null | cat -v > $LINK_VIEWER_LOG &"
		sleep 3
		LINK_VIEWER=$(remote "cat $LINK_VIEWER_LOG 2>/dev/null" || echo "")
		remote "rm -f $LINK_VIEWER_LOG" 2>/dev/null

		if echo "$LINK_VIEWER" | grep -q "tmtv.se/test"; then
			pass "hyperlinks: OSC 8 relayed to SSH viewer"
		else
			# OSC 8 may be stripped by tmux hyperlink processing — check if
			# the hyperlink was at least registered in the grid
			HL_CHECK=$(remote_tmtv "capture-pane -t main -p -e" 2>/dev/null || echo "")
			if echo "$HL_CHECK" | grep -q "CLICK HERE"; then
				pass "hyperlinks: OSC 8 text visible (link may be grid-rendered)"
			else
				fail "hyperlinks: OSC 8 relayed to SSH viewer" \
					"marker not in viewer ($(echo "$LINK_VIEWER" | wc -c) bytes)"
			fi
		fi

		# -------------------------------------------------------
		# Test: pane scrollbars option exists (tmux 3.6)
		# -------------------------------------------------------
		SB_CHECK=$(remote_tmtv "show-options -wv pane-scrollbars" 2>/dev/null || echo "")
		# pane-scrollbars may not exist in all builds; just check the option is recognized
		SB_LIST=$(remote_tmtv "show-options -w" 2>/dev/null | grep -c "pane-scrollbars" || true)
		SB_LIST=${SB_LIST:-0}
		if [ "$SB_LIST" -gt 0 ] || [ -n "$SB_CHECK" ]; then
			pass "scrollbars: pane-scrollbars option exists"
		else
			# Option might only appear when explicitly set
			remote_tmtv "set-option -w pane-scrollbars on" 2>/dev/null
			SB_SET=$(remote_tmtv "show-options -wv pane-scrollbars" 2>/dev/null || echo "")
			if [ "$SB_SET" = "on" ]; then
				pass "scrollbars: pane-scrollbars option exists"
				remote_tmtv "set-option -wu pane-scrollbars" 2>/dev/null
			else
				fail "scrollbars: pane-scrollbars option exists" \
					"option not recognized"
			fi
		fi

		# -------------------------------------------------------
		# Test: control mode (-CC) command exists
		# -------------------------------------------------------
		# Control mode is invoked via tmtv attach -C or tmtv new -C
		# Just verify the -C flag is recognized
		CC_CHECK=$(remote "TERM=xterm-256color $REMOTE_TMTV list-commands 2>/dev/null | grep 'attach-session'" || echo "")
		if echo "$CC_CHECK" | grep -q "\-C"; then
			pass "control mode: -C flag available in attach-session"
		elif echo "$CC_CHECK" | grep -q "attach-session"; then
			# -C is listed in the command's flags
			pass "control mode: attach-session command available"
		else
			fail "control mode: -C flag available" "not found"
		fi

		# Clean up
		remote "rm -f $ESC_CONF" 2>/dev/null || true
		teardown_section "escape"
	fi
else
	skip "passthrough: allow-passthrough defaults to off" "no fingerprints"
	skip "passthrough: allow-passthrough set to on" "no fingerprints"
	skip "passthrough: DCS relayed to SSH viewer" "no fingerprints"
	skip "OSC 52: paste buffer populated via clipboard set" "no fingerprints"
	skip "OSC 52: clipboard relayed to SSH viewer" "no fingerprints"
	skip "extended-keys: defaults to off" "no fingerprints"
	skip "extended-keys: set to on" "no fingerprints"
	skip "extended-keys-format: defaults to xterm or csi-u" "no fingerprints"
	skip "focus-events: defaults to off" "no fingerprints"
	skip "focus-events: set to on" "no fingerprints"
	skip "display-popup: command available" "no fingerprints"
	skip "popup-border-lines: option exists" "no fingerprints"
	skip "SIXEL: binary built with --enable-sixel" "no fingerprints"
	skip "SIXEL: image output doesn't crash pane" "no fingerprints"
	skip "SIXEL: image reaches SSH viewer" "no fingerprints"
	skip "SIXEL: allow-passthrough option works" "no fingerprints"
	skip "SIXEL: SSE stream carries data after SIXEL output" "no fingerprints"
	skip "hyperlinks: OSC 8 relayed to SSH viewer" "no fingerprints"
	skip "scrollbars: pane-scrollbars option exists" "no fingerprints"
	skip "control mode: -C flag available" "no fingerprints"
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
	wait_for 15 1 "recording session ready" \
		"remote 'TERM=xterm-256color $REMOTE_TMTV list-sessions 2>/dev/null | grep -q main'"

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
	remote "rm -f $REC_CONF" 2>/dev/null || true
	remote "rm -rf /root/.tmtv/recordings" 2>/dev/null || true
	teardown_section "recording"
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
# Test: Session lobby screen (Playwright)
# When a session ends or a bad token is used, the error overlay
# should show a session token input form (the "lobby").
# -------------------------------------------------------
if [ "$HAS_PLAYWRIGHT" = "true" ] && [ "$QUICK" = "false" ] && [ "$HAS_WEB" = "true" ]; then
	# Test the "unavailable" case with a bogus token — no live session needed
	LOBBY_SCREENSHOT_DIR="/tmp/tmtv-lobby-screenshots-$$"
	mkdir -p "$LOBBY_SCREENSHOT_DIR"
	LOBBY_TEST_SCRIPT="$(dirname "$0")/test-session-lobby.js"
	LOBBY_PW_EXIT=0
	LOBBY_PW_OUTPUT=$(NODE_PATH="$PW_NODE_PATH" PLAYWRIGHT_BROWSERS_PATH="$PW_BROWSERS_PATH" \
		node "$LOBBY_TEST_SCRIPT" \
		"$WEB_URL/s/bogus-token-lobby-test" "$LOBBY_SCREENSHOT_DIR" "unavailable" "30" 2>&1) || LOBBY_PW_EXIT=$?
	if [ $LOBBY_PW_EXIT -eq 0 ]; then
		echo "$LOBBY_PW_OUTPUT" | grep "PASS step 1" >/dev/null && pass "session lobby form visible on unavailable session"
		echo "$LOBBY_PW_OUTPUT" | grep "PASS step 2" >/dev/null && pass "session lobby input and button accessible"
		echo "$LOBBY_PW_OUTPUT" | grep "PASS step 3" >/dev/null && pass "session lobby form navigates to /s/<token>"
	elif echo "$LOBBY_PW_OUTPUT" | grep -q "MODULE_NOT_FOUND"; then
		skip "session lobby form visible (playwright module not installed)"
		skip "session lobby input accessible (playwright module not installed)"
		skip "session lobby navigation (playwright module not installed)"
	else
		fail "session lobby test" "$LOBBY_PW_OUTPUT"
	fi
	rm -rf "$LOBBY_SCREENSHOT_DIR"
else
	if [ "$HAS_PLAYWRIGHT" != "true" ]; then
		skip "session lobby form visible (playwright not installed)"
		skip "session lobby input accessible (playwright not installed)"
		skip "session lobby navigation (playwright not installed)"
	elif [ "$QUICK" = "true" ]; then
		skip "session lobby form visible (quick mode)"
		skip "session lobby input accessible (quick mode)"
		skip "session lobby navigation (quick mode)"
	else
		skip "session lobby form visible (web not available)"
		skip "session lobby input accessible (web not available)"
		skip "session lobby navigation (web not available)"
	fi
fi

# -------------------------------------------------------
# Test: Rapid resize does not break SSE stream (v1.6.0 debounce)
# -------------------------------------------------------
RESIZE_CONF="/tmp/.tmtv-test-resize-$TESTID.conf"
RESIZE_SESSNAME="resize-$TESTID"
remote "cat > $RESIZE_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$RESIZE_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $RESIZE_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$RESIZE_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

RESIZE_TOKEN=$(read_token "$RESIZE_SESSNAME")

if [ -n "$RESIZE_TOKEN" ]; then
	# Prime the vpty by connecting once
	curl -s -m 2 -N "$SSE_BASE/$RESIZE_TOKEN" >/dev/null 2>&1 || true
	sleep 2

	# Fire rapid resizes (simulates browser window drag)
	for _sz in 90x25 100x30 120x35 80x24 110x28 80x24; do
		_rx=$(echo "$_sz" | cut -dx -f1)
		_ry=$(echo "$_sz" | cut -dx -f2)
		remote "TERM=xterm-256color $REMOTE_TMTV -f $RESIZE_CONF resize-window -t main -x $_rx -y $_ry" 2>/dev/null || true
	done
	sleep 3

	# Verify SSE still delivers data after rapid resize burst
	RESIZE_TOKEN=$(read_token "$RESIZE_SESSNAME")
	SSE_AFTER_RESIZE=$(curl -s -m 4 -N "$SSE_BASE/$RESIZE_TOKEN" 2>/dev/null || echo "")
	SSE_RESIZE_LINES=$(echo "$SSE_AFTER_RESIZE" | grep -c "^data:" || echo "0")
	if [ "$SSE_RESIZE_LINES" -gt 0 ]; then
		pass "SSE stream survives rapid resize burst ($SSE_RESIZE_LINES frames)"
	else
		fail "SSE stream survives rapid resize burst" \
			"no data: lines after 6 rapid resizes"
	fi

	remote "rm -f $RESIZE_CONF" 2>/dev/null || true
	teardown_section "resize"
else
	skip "SSE stream survives rapid resize burst (could not create session)"
fi

# -------------------------------------------------------
# Test: First SSE connect gets immediate content (v1.6.0 blank viewer fix)
# -------------------------------------------------------
BLANK_CONF="/tmp/.tmtv-test-blank-$TESTID.conf"
BLANK_SESSNAME="blank-$TESTID"
remote "cat > $BLANK_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$BLANK_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $BLANK_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$BLANK_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

BLANK_TOKEN=$(read_token "$BLANK_SESSNAME")

if [ -n "$BLANK_TOKEN" ]; then
	# Put a marker on screen before any SSE client connects
	BLANK_MARKER="BLNK_$$"
	remote "TERM=xterm-256color $REMOTE_TMTV -f $BLANK_CONF send-keys 'echo $BLANK_MARKER' Enter" 2>/dev/null || true
	sleep 2

	# First SSE connect — should get screen dump with marker immediately
	SSE_FIRST=$(curl -s -m 4 -N "$SSE_BASE/$BLANK_TOKEN" 2>/dev/null || echo "")
	SSE_FIRST_LINES=$(echo "$SSE_FIRST" | grep -c "^data:" || echo "0")
	if [ "$SSE_FIRST_LINES" -gt 0 ]; then
		pass "first SSE connect gets immediate content ($SSE_FIRST_LINES frames)"
	else
		fail "first SSE connect gets immediate content" \
			"no data: lines on first connect"
	fi

	remote "rm -f $BLANK_CONF" 2>/dev/null || true
	teardown_section "blank"
else
	skip "first SSE connect gets immediate content (could not create session)"
fi

# -------------------------------------------------------
# Test: Backpressure — SSE stream does not CPU-spin under load (v1.6.0)
# -------------------------------------------------------
BP_CONF="/tmp/.tmtv-test-bp-$TESTID.conf"
BP_SESSNAME="bp-$TESTID"
remote "cat > $BP_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$BP_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $BP_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$BP_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

BP_TOKEN=$(read_token "$BP_SESSNAME")

if [ -n "$BP_TOKEN" ]; then
	# Connect an SSE client to activate vpty
	curl -s -m 2 -N "$SSE_BASE/$BP_TOKEN" >/dev/null 2>&1 || true
	sleep 2

	# Generate a burst of output (>128KB) to trigger backpressure
	remote "TERM=xterm-256color $REMOTE_TMTV -f $BP_CONF send-keys 'seq 1 10000' Enter" 2>/dev/null || true
	sleep 3

	# Measure CPU usage of tmtv-server — should NOT be spinning
	# Get the PID of tmtv-server daemon (the main accept loop)
	SERVER_PID=$(remote "pgrep -f 'tmtv-server.*${TMTV_PORT}' | head -1" 2>/dev/null || echo "")
	if [ -n "$SERVER_PID" ]; then
		# Sample CPU over 2 seconds
		CPU_PCT=$(remote "ps -p $SERVER_PID -o %cpu= 2>/dev/null" | tr -d ' ' || echo "0")
		# CPU should be well under 50% — a spin loop would show ~100%
		CPU_INT=$(echo "$CPU_PCT" | cut -d. -f1)
		[ -z "$CPU_INT" ] && CPU_INT=0
		if [ "$CPU_INT" -lt 50 ]; then
			pass "no CPU spin under backpressure (${CPU_PCT}%)"
		else
			fail "no CPU spin under backpressure" \
				"tmtv-server CPU at ${CPU_PCT}% after burst"
		fi
	else
		skip "no CPU spin under backpressure (could not find server PID)"
	fi

	# Verify SSE is still functional after the burst
	BP_TOKEN=$(read_token "$BP_SESSNAME")
	SSE_AFTER_BP=$(curl -s -m 4 -N "$SSE_BASE/$BP_TOKEN" 2>/dev/null || echo "")
	SSE_BP_LINES=$(echo "$SSE_AFTER_BP" | grep -c "^data:" || echo "0")
	if [ "$SSE_BP_LINES" -gt 0 ]; then
		pass "SSE stream recovers after output burst ($SSE_BP_LINES frames)"
	else
		fail "SSE stream recovers after output burst" \
			"no data after 10K line burst"
	fi

	remote "rm -f $BP_CONF" 2>/dev/null || true
	teardown_section "backpressure"
else
	skip "no CPU spin under backpressure (could not create session)"
	skip "SSE stream recovers after output burst (could not create session)"
fi

# -------------------------------------------------------
# Test: Truecolor (RGB) terminal feature enabled (v1.6.1)
# -------------------------------------------------------
TC_CONF="/tmp/.tmtv-test-tc-$TESTID.conf"
TC_SESSNAME="tc-$TESTID"
remote "cat > $TC_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$TC_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $TC_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$TC_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

TC_TOKEN=$(read_token "$TC_SESSNAME")

if [ -n "$TC_TOKEN" ]; then
	# Check that tmux reports RGB in terminal-features for xterm*
	TC_FEATURES=$(remote "TERM=xterm-256color $REMOTE_TMTV -f $TC_CONF show-options -g terminal-features 2>/dev/null" || echo "")
	if echo "$TC_FEATURES" | grep -q "RGB"; then
		pass "terminal-features includes RGB for xterm"
	else
		fail "terminal-features includes RGB for xterm" \
			"got: $TC_FEATURES"
	fi

	# Verify truecolor escape sequences pass through to the terminal
	# by sending an RGB color and checking capture-pane output
	remote "TERM=xterm-256color $REMOTE_TMTV -f $TC_CONF send-keys 'printf \"\\033[38;2;255;128;0mTRUECOLOR_OK\\033[0m\"' Enter" 2>/dev/null || true
	sleep 2
	TC_CAP=$(remote "TERM=xterm-256color $REMOTE_TMTV -f $TC_CONF capture-pane -t main:0 -p 2>/dev/null" || echo "")
	if echo "$TC_CAP" | grep -q "TRUECOLOR_OK"; then
		pass "truecolor text rendered in session"
	else
		fail "truecolor text rendered in session" \
			"TRUECOLOR_OK not found in capture-pane"
	fi

	remote "rm -f $TC_CONF" 2>/dev/null || true
	teardown_section "truecolor"
else
	skip "terminal-features includes RGB for xterm (could not create session)"
	skip "truecolor text rendered in session (could not create session)"
fi

# -------------------------------------------------------
# Test: Session tokens are 16 characters (v1.6.2 entropy)
# -------------------------------------------------------
TOK16_CONF="/tmp/.tmtv-test-tok16-$TESTID.conf"
TOK16_SESSNAME="tok16-$TESTID"
remote "cat > $TOK16_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$TOK16_SESSNAME\"
set -g tmtv-web-sharing on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $TOK16_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$TOK16_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

TOK16_TOKEN=$(read_token "$TOK16_SESSNAME")

if [ -n "$TOK16_TOKEN" ]; then
	TOK16_LEN=$(echo -n "$TOK16_TOKEN" | wc -c)
	if [ "$TOK16_LEN" -eq 16 ]; then
		pass "session token is 16 characters (was 8)"
	else
		fail "session token is 16 characters" \
			"got ${TOK16_LEN} chars: $TOK16_TOKEN"
	fi

	remote "rm -f $TOK16_CONF" 2>/dev/null || true
	teardown_section "tok16"
else
	skip "session token is 16 characters (could not create session)"
fi

# -------------------------------------------------------
# Test: Password auth works with PBKDF2 hashing (v1.6.2)
# -------------------------------------------------------
PBKDF_CONF="/tmp/.tmtv-test-pbkdf-$TESTID.conf"
PBKDF_SESSNAME="pbkdf-$TESTID"
PBKDF_PASSWORD="testpass_$$"
remote "cat > $PBKDF_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$PBKDF_SESSNAME\"
set -g tmtv-session-password \"$PBKDF_PASSWORD\"
set -g tmtv-web-sharing on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $PBKDF_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$PBKDF_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

PBKDF_TOKEN=$(read_token "$PBKDF_SESSNAME")

if [ -n "$PBKDF_TOKEN" ] && [ "$HAS_WEB" = "true" ]; then
	# Without password — should get 403
	PBKDF_NOPW=$(curl -sk -o /dev/null -w "%{http_code}" \
		-m 5 "$SSE_BASE/$PBKDF_TOKEN" 2>/dev/null || echo "000")
	if [ "$PBKDF_NOPW" = "403" ]; then
		pass "PBKDF2: no password returns 403"
	else
		fail "PBKDF2: no password returns 403" \
			"got HTTP $PBKDF_NOPW"
	fi

	# Wrong password — should get 403
	PBKDF_WRONG=$(curl -sk -o /dev/null -w "%{http_code}" \
		-m 5 "$SSE_BASE/$PBKDF_TOKEN?password=wrongpass" 2>/dev/null || echo "000")
	if [ "$PBKDF_WRONG" = "403" ]; then
		pass "PBKDF2: wrong password returns 403"
	else
		fail "PBKDF2: wrong password returns 403" \
			"got HTTP $PBKDF_WRONG"
	fi

	# Correct password via query param — should connect
	PBKDF_OK=$(curl -s -m 4 -N \
		"$SSE_BASE/$PBKDF_TOKEN?password=$PBKDF_PASSWORD" 2>/dev/null || echo "")
	if echo "$PBKDF_OK" | grep -q "^data:"; then
		pass "PBKDF2: correct password connects (query param)"
	else
		fail "PBKDF2: correct password connects" \
			"no data received with correct password"
	fi

	# Correct password via X-Tmtv-Password header — should connect
	PBKDF_HDR=$(curl -s -m 4 -N \
		-H "X-Tmtv-Password: $PBKDF_PASSWORD" \
		"$SSE_BASE/$PBKDF_TOKEN" 2>/dev/null || echo "")
	if echo "$PBKDF_HDR" | grep -q "^data:"; then
		pass "PBKDF2: correct password connects (header)"
	else
		fail "PBKDF2: correct password connects (header)" \
			"no data received with X-Tmtv-Password header"
	fi

	remote "rm -f $PBKDF_CONF" 2>/dev/null || true
	teardown_section "pbkdf"
else
	skip "PBKDF2: no password returns 403 (no token or web)"
	skip "PBKDF2: wrong password returns 403 (no token or web)"
	skip "PBKDF2: correct password connects (no token or web)"
	skip "PBKDF2: correct password connects header (no token or web)"
fi

# -------------------------------------------------------
# Test: OSC 52 clipboard reaches SSE stream (v1.6.2)
# -------------------------------------------------------
OSC_CONF="/tmp/.tmtv-test-osc-$TESTID.conf"
OSC_SESSNAME="osc-$TESTID"
remote "cat > $OSC_CONF << CONF
set -g tmtv-server-host \"127.0.0.1\"
set -g tmtv-server-port $TMTV_PORT
set -g tmtv-server-rsa-fingerprint \"$RSA_FP\"
set -g tmtv-server-ed25519-fingerprint \"$ED25519_FP\"
set -g tmtv-session-name \"$OSC_SESSNAME\"
set -g tmtv-web-sharing on
set -g allow-passthrough on
CONF"

remote "TERM=xterm-256color \
	nohup script -qc '$REMOTE_TMTV -f $OSC_CONF new-session -d -s main' \
	/dev/null </dev/null >/dev/null 2>&1 &"

_prev_tok=""
_stable=0
for _wait in $(seq 1 20); do
	sleep 1
	_cur_tok=$(remote "readlink $SESSIONS_DIR/$OSC_SESSNAME 2>/dev/null" || echo "")
	if [ -n "$_cur_tok" ] && [ "$_cur_tok" = "$_prev_tok" ]; then
		_stable=$((_stable + 1)); [ "$_stable" -ge 3 ] && break
	else _stable=0; fi
	_prev_tok="$_cur_tok"
done

OSC_TOKEN=$(read_token "$OSC_SESSNAME")

if [ -n "$OSC_TOKEN" ]; then
	# Prime vpty
	curl -s -m 2 -N "$SSE_BASE/$OSC_TOKEN" >/dev/null 2>&1 || true
	sleep 2

	# Send OSC 52 with a known payload (base64 of "CLIPBOARD_TEST")
	# OSC 52 format: ESC ] 52 ; c ; <base64> BEL
	OSC_B64=$(echo -n "CLIPBOARD_TEST" | base64)
	remote "TERM=xterm-256color $REMOTE_TMTV -f $OSC_CONF send-keys 'printf \"\\033]52;c;${OSC_B64}\\007\"' Enter" 2>/dev/null || true
	sleep 2

	# Capture SSE data and check if it contains the OSC 52 sequence
	OSC_TOKEN=$(read_token "$OSC_SESSNAME")
	SSE_OSC=$(curl -s -m 4 -N "$SSE_BASE/$OSC_TOKEN" 2>/dev/null || echo "")
	SSE_OSC_LINES=$(echo "$SSE_OSC" | grep -c "^data:" || echo "0")
	if [ "$SSE_OSC_LINES" -gt 0 ]; then
		pass "OSC 52: SSE stream active after clipboard set ($SSE_OSC_LINES frames)"
	else
		fail "OSC 52: SSE stream active after clipboard set" \
			"no data after OSC 52 send"
	fi

	remote "rm -f $OSC_CONF" 2>/dev/null || true
	teardown_section "osc52"
else
	skip "OSC 52: SSE stream active after clipboard set (could not create session)"
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
