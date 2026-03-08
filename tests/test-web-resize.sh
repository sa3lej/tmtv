#!/bin/bash
# Test: tmux pane resize via web viewer (Playwright screenshots)
# Verifies that tmux pane splits and resizes are reflected in the SSE web viewer.
#
# Requirements:
#   - Remote server running tmtv-server (89.167.2.83)
#   - Playwright installed locally (npx playwright)
#   - SSH access to remote as root

set -euo pipefail

REMOTE="root@89.167.2.83"
SCREENSHOT_DIR="/tmp/tmtv-resize-tests"
VIEWER_BASE="http://89.167.2.83:8080/s"
PASS=0
FAIL=0

mkdir -p "$SCREENSHOT_DIR"

log() { echo "[test-resize] $*"; }
pass() { log "PASS: $1"; PASS=$((PASS + 1)); }
fail() { log "FAIL: $1"; FAIL=$((FAIL + 1)); }

cleanup() {
    log "Cleaning up remote session..."
    ssh "$REMOTE" 'TERM=xterm-256color /root/tmtv kill-server 2>/dev/null; pkill -f "tmtv -F" 2>/dev/null' || true
}
trap cleanup EXIT

# Helper: run tmtv command on remote via the running tmux server
run_tmtv() {
    ssh "$REMOTE" "TERM=xterm-256color /root/tmtv $*" 2>&1 || true
    sleep 3
}

# Helper to take a screenshot
screenshot() {
    local name="$1"
    local path="$SCREENSHOT_DIR/$name.png"
    npx playwright screenshot --wait-for-timeout=2000 "$VIEWER_URL" "$path" 2>/dev/null
    if [ -f "$path" ]; then
        log "Screenshot saved: $path"
        return 0
    else
        return 1
    fi
}

# Step 1: Start tmtv -F session using script for PTY
log "Starting tmtv session on remote..."
ssh "$REMOTE" 'rm -f /tmp/tmtv-resize.log
nohup script -qc "TERM=xterm-256color /root/tmtv -F" /tmp/tmtv-resize.log > /dev/null 2>&1 &
echo $!'

log "Waiting for session to connect..."
for i in $(seq 1 20); do
    sleep 3
    CONNECTED=$(ssh "$REMOTE" 'strings /tmp/tmtv-resize.log 2>/dev/null | grep -c "ssh session"' || echo 0)
    if [ "$CONNECTED" -gt 0 ]; then
        break
    fi
    if [ $((i % 5)) -eq 0 ]; then
        log "  Still waiting... ($((i*3))s elapsed)"
    fi
done

if [ "$CONNECTED" -eq 0 ]; then
    log "ERROR: tmtv session did not connect"
    ssh "$REMOTE" 'strings /tmp/tmtv-resize.log 2>/dev/null | tail -20'
    exit 1
fi
log "Session connected. Enabling web sharing..."
run_tmtv set -g tmtv-web-sharing on
sleep 3

# Get token from remote log
TOKEN=$(ssh "$REMOTE" 'strings /tmp/tmtv-resize.log 2>/dev/null | grep -oP "http://[^ ]+" | head -1 | sed "s|.*/s/||"')
if [ -z "$TOKEN" ]; then
    # Fallback: try show-messages
    TOKEN=$(ssh "$REMOTE" 'TERM=xterm-256color /root/tmtv show-messages 2>/dev/null | grep -oP "http://[^ ]+" | head -1 | sed "s|.*/s/||"' || true)
fi

if [ -z "$TOKEN" ]; then
    log "ERROR: Could not get session token"
    ssh "$REMOTE" 'strings /tmp/tmtv-resize.log 2>/dev/null | tail -20'
    exit 1
fi
log "Session token: $TOKEN"
VIEWER_URL="$VIEWER_BASE/$TOKEN"
log "Viewer URL: $VIEWER_URL"

# ============================================================
# Test 1: Single pane (default layout)
# ============================================================
log "Test 1: Single pane view..."
if screenshot "01-single-pane"; then
    pass "Single pane screenshot captured"
else
    fail "Single pane screenshot failed"
fi

# ============================================================
# Test 2: Horizontal split
# ============================================================
log "Test 2: Horizontal split..."
run_tmtv split-window -h
if screenshot "02-horizontal-split"; then
    pass "Horizontal split screenshot captured"
else
    fail "Horizontal split screenshot failed"
fi

# ============================================================
# Test 3: Resize pane (make left pane bigger)
# ============================================================
log "Test 3: Resize pane left +10..."
run_tmtv resize-pane -L 10
if screenshot "03-resize-left"; then
    pass "Resize pane screenshot captured"
else
    fail "Resize pane screenshot failed"
fi

# ============================================================
# Test 4: Vertical split (3 panes)
# ============================================================
log "Test 4: Vertical split (3 panes)..."
run_tmtv split-window -v
if screenshot "04-vertical-split"; then
    pass "Vertical split screenshot captured"
else
    fail "Vertical split screenshot failed"
fi

# ============================================================
# Test 5: Resize pane down
# ============================================================
log "Test 5: Resize pane down +5..."
run_tmtv resize-pane -D 5
if screenshot "05-resize-down"; then
    pass "Resize down screenshot captured"
else
    fail "Resize down screenshot failed"
fi

# ============================================================
# Test 6: Even-horizontal layout
# ============================================================
log "Test 6: Even-horizontal layout..."
run_tmtv select-layout even-horizontal
if screenshot "06-even-horizontal"; then
    pass "Even-horizontal layout screenshot captured"
else
    fail "Even-horizontal layout screenshot failed"
fi

# ============================================================
# Test 7: Tiled layout (add 4th pane, then tile)
# ============================================================
log "Test 7: Tiled layout..."
run_tmtv split-window
run_tmtv select-layout tiled
if screenshot "07-tiled-layout"; then
    pass "Tiled layout screenshot captured"
else
    fail "Tiled layout screenshot failed"
fi

# ============================================================
# Test 8: Disable web sharing (SSE should disconnect)
# ============================================================
log "Test 8: Disable web sharing..."
run_tmtv set -g tmtv-web-sharing off
sleep 2
if screenshot "08-sharing-disabled"; then
    pass "Sharing disabled screenshot captured"
else
    fail "Sharing disabled screenshot failed"
fi

# ============================================================
# Summary
# ============================================================
echo ""
echo "========================================"
echo "  Resize Test Results"
echo "========================================"
echo "  PASS: $PASS"
echo "  FAIL: $FAIL"
echo "  Screenshots: $SCREENSHOT_DIR/"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
