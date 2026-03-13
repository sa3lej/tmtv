#!/bin/sh
#
# Safe, idempotent staging deploy for tmtv.
#
# Automates: build -> preflight checks -> deploy -> verify -> test -> report.
# Designed for both human operators and AI agents. Every step logs what it
# does, fails loudly on errors, and can be re-run safely.
#
# Usage:
#   scripts/staging-deploy.sh                # full deploy: build + binary + web + verify + test
#   scripts/staging-deploy.sh --binary-only  # build and deploy binaries only
#   scripts/staging-deploy.sh --web-only     # build and deploy web assets only
#   scripts/staging-deploy.sh --test-only    # run integration tests only (no deploy)
#   scripts/staging-deploy.sh --skip-build   # deploy existing binaries (no rebuild)
#   scripts/staging-deploy.sh --skip-tests   # deploy but skip integration tests
#   scripts/staging-deploy.sh --full-tests   # run full test suite (includes Playwright)
#
# Environment:
#   STAGING_HOST     - staging server hostname (default: staging.tmtv.se)
#   STAGING_USER     - SSH user (default: ubuntu)
#   STAGING_SSH_PORT - SSH port for admin access (default: 22)
#
# Exit codes:
#   0 - success
#   1 - preflight check failed
#   2 - build failed
#   3 - deploy failed
#   4 - verification failed
#   5 - integration tests failed
#

set -e

# --- Configuration ---

STAGING_HOST="${STAGING_HOST:-staging.tmtv.se}"
STAGING_USER="${STAGING_USER:-ubuntu}"
STAGING_SSH_PORT="${STAGING_SSH_PORT:-22}"
STAGING_TARGET="${STAGING_USER}@${STAGING_HOST}"

SSH_OPTS="-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -p ${STAGING_SSH_PORT}"
SCP_OPTS="-o StrictHostKeyChecking=accept-new -o ConnectTimeout=10 -P ${STAGING_SSH_PORT}"

# Resolve repo root (works regardless of cwd)
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# --- Parse flags ---

MODE="full"           # full | binary-only | web-only | test-only
SKIP_BUILD=false
SKIP_TESTS=false
FULL_TESTS=false

for arg in "$@"; do
    case "$arg" in
        --binary-only) MODE="binary-only" ;;
        --web-only)    MODE="web-only" ;;
        --test-only)   MODE="test-only" ;;
        --skip-build)  SKIP_BUILD=true ;;
        --skip-tests)  SKIP_TESTS=true ;;
        --full-tests)  FULL_TESTS=true ;;
        --help|-h)
            sed -n '2,/^$/{ s/^# //; s/^#$//; p }' "$0"
            exit 0
            ;;
        *)
            echo "ERROR: Unknown flag: $arg"
            echo "Run with --help for usage."
            exit 1
            ;;
    esac
done

# --- Logging helpers ---

step() {
    echo ""
    echo "==> $1"
}

info() {
    echo "    $1"
}

ok() {
    echo "    OK: $1"
}

err() {
    echo "    ERROR: $1" >&2
}

# --- Remote execution helper ---

remote() {
    ssh $SSH_OPTS "$STAGING_TARGET" "$@"
}

# --- Step 1: Preflight checks ---

step "Preflight checks"

# Check staging server is reachable
if ! ssh $SSH_OPTS "$STAGING_TARGET" "echo ok" >/dev/null 2>&1; then
    err "Cannot reach ${STAGING_TARGET} on port ${STAGING_SSH_PORT}"
    err "Check SSH access and VPN/network connectivity."
    exit 1
fi
ok "Staging server reachable"

# Check tmtv-server service exists
if ! remote "sudo systemctl cat tmtv-server" >/dev/null 2>&1; then
    err "tmtv-server systemd service not found on staging"
    exit 1
fi
ok "tmtv-server systemd service exists"

# Check for active sessions (warning, not blocking)
HEALTH=$(remote "curl -s -m 3 http://127.0.0.1:4002/healthz" 2>/dev/null || echo "")
if echo "$HEALTH" | grep -q '"active_sessions"'; then
    SESSIONS=$(echo "$HEALTH" | sed 's/.*"active_sessions":\([0-9]*\).*/\1/')
    if [ "$SESSIONS" -gt 0 ] 2>/dev/null; then
        echo "    WARNING: $SESSIONS active session(s) on staging — deploy will restart the server"
    else
        ok "No active sessions"
    fi
else
    info "Could not check active sessions (server may be down — will restart it)"
fi

# Record pre-deploy version for comparison
PRE_SERVER_VER=$(remote "tmtv-server -V 2>&1" || echo "unknown")
PRE_CLIENT_VER=$(remote "TERM=xterm-256color /usr/local/bin/tmtv -V 2>&1" || echo "unknown")
info "Current server: $PRE_SERVER_VER"
info "Current client: $PRE_CLIENT_VER"

# --- Step 2: Build ---

if [ "$MODE" = "test-only" ]; then
    info "Skipping build (--test-only)"
elif [ "$SKIP_BUILD" = "true" ]; then
    info "Skipping build (--skip-build)"
elif [ "$MODE" = "web-only" ]; then
    info "Skipping binary build (--web-only)"
else
    step "Building tmtv"
    cd "$REPO_ROOT"

    if [ ! -f configure ]; then
        info "Running autogen.sh..."
        sh autogen.sh || { err "autogen.sh failed"; exit 2; }
    fi

    if [ ! -f Makefile ]; then
        info "Running configure..."
        ./configure || { err "configure failed"; exit 2; }
    fi

    NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
    make -j"$NPROC" || { err "make failed"; exit 2; }

    info "Built: $(./tmtv -V 2>&1)"
    info "Built: $(./tmtv-server -V 2>&1)"

    # Sanity check: binaries exist and are executable
    if [ ! -x "$REPO_ROOT/tmtv" ] || [ ! -x "$REPO_ROOT/tmtv-server" ]; then
        err "Build produced missing or non-executable binaries"
        exit 2
    fi
    ok "Build succeeded"
fi

# --- Step 3: Deploy binaries ---

if [ "$MODE" = "full" ] || [ "$MODE" = "binary-only" ]; then
    step "Deploying binaries"

    # Upload to /tmp/ first (avoids "text file busy" errors)
    scp $SCP_OPTS "$REPO_ROOT/tmtv" "${STAGING_TARGET}:/tmp/tmtv-bin" || { err "SCP tmtv failed"; exit 3; }
    scp $SCP_OPTS "$REPO_ROOT/tmtv-server" "${STAGING_TARGET}:/tmp/tmtv-server-bin" || { err "SCP tmtv-server failed"; exit 3; }
    ok "Binaries uploaded to /tmp/"

    # Stop, replace, start
    remote "sudo systemctl stop tmtv-server" || { err "Failed to stop tmtv-server"; exit 3; }
    info "tmtv-server stopped"

    remote "sudo mv -f /tmp/tmtv-bin /usr/local/bin/tmtv && \
            sudo mv -f /tmp/tmtv-server-bin /usr/local/bin/tmtv-server && \
            sudo chmod +x /usr/local/bin/tmtv /usr/local/bin/tmtv-server" || { err "Failed to install binaries"; exit 3; }
    info "Binaries installed"

    remote "sudo systemctl start tmtv-server" || { err "Failed to start tmtv-server"; exit 3; }
    info "tmtv-server started"

    # Give server a moment to initialize
    sleep 2

    ok "Binary deploy complete"
fi

# --- Step 4: Deploy web assets ---

if [ "$MODE" = "full" ] || [ "$MODE" = "web-only" ]; then
    step "Deploying web assets"

    # Build Astro site
    cd "$REPO_ROOT/site"
    if [ ! -d node_modules ]; then
        info "Installing npm dependencies..."
        npm ci || { err "npm ci failed"; exit 3; }
    fi
    info "Building Astro site..."
    npm run build --silent || { err "Astro build failed"; exit 3; }

    # Upload files to /tmp/
    scp $SCP_OPTS "$REPO_ROOT/site/dist/index.html" "${STAGING_TARGET}:/tmp/index.html" || { err "SCP index.html failed"; exit 3; }
    scp $SCP_OPTS "$REPO_ROOT/site/dist/viewer/index.html" "${STAGING_TARGET}:/tmp/viewer.html" || { err "SCP viewer.html failed"; exit 3; }
    scp $SCP_OPTS "$REPO_ROOT/web/viewer.js" "${STAGING_TARGET}:/tmp/viewer.js" || { err "SCP viewer.js failed"; exit 3; }

    # Upload static assets (optional, non-fatal)
    for f in "$REPO_ROOT"/site/dist/*.txt "$REPO_ROOT"/site/dist/*.png "$REPO_ROOT"/site/dist/*.gif "$REPO_ROOT"/site/dist/favicon.*; do
        [ -f "$f" ] && scp $SCP_OPTS "$f" "${STAGING_TARGET}:/tmp/" 2>/dev/null || true
    done

    # Move into web root atomically
    remote "sudo mkdir -p /var/www/tmtv && \
            sudo mv -f /tmp/index.html /var/www/tmtv/index.html && \
            sudo mv -f /tmp/viewer.html /var/www/tmtv/viewer.html && \
            sudo mv -f /tmp/viewer.js /var/www/tmtv/viewer.js && \
            for f in /tmp/*.txt /tmp/*.png /tmp/*.gif /tmp/favicon.*; do \
                [ -f \"\$f\" ] && sudo mv -f \"\$f\" /var/www/tmtv/ || true; \
            done" || { err "Failed to install web assets"; exit 3; }

    ok "Web assets deployed"
fi

# --- Step 5: Verify deployment ---

if [ "$MODE" != "test-only" ]; then
    step "Verifying deployment"

    # Check systemd service
    if remote "sudo systemctl is-active tmtv-server" >/dev/null 2>&1; then
        ok "tmtv-server is active"
    else
        err "tmtv-server is not active after deploy"
        remote "sudo journalctl -u tmtv-server -n 20 --no-pager" 2>/dev/null || true
        exit 4
    fi

    # Check version
    POST_SERVER_VER=$(remote "tmtv-server -V 2>&1" || echo "unknown")
    POST_CLIENT_VER=$(remote "TERM=xterm-256color /usr/local/bin/tmtv -V 2>&1" || echo "unknown")
    info "Server: $PRE_SERVER_VER -> $POST_SERVER_VER"
    info "Client: $PRE_CLIENT_VER -> $POST_CLIENT_VER"

    # Health endpoint
    POST_HEALTH=$(remote "curl -s -m 5 http://127.0.0.1:4002/healthz" 2>/dev/null || echo "")
    if echo "$POST_HEALTH" | grep -q '"status":"ok"'; then
        ok "Health endpoint: $POST_HEALTH"
    else
        err "Health endpoint not responding: $POST_HEALTH"
        exit 4
    fi

    # Web endpoints (non-fatal — Caddy might need a moment)
    LANDING_STATUS=$(remote "curl -s -o /dev/null -w '%{http_code}' https://staging.tmtv.se/" 2>/dev/null || echo "000")
    VIEWER_STATUS=$(remote "curl -s -o /dev/null -w '%{http_code}' https://staging.tmtv.se/s/test" 2>/dev/null || echo "000")
    if [ "$LANDING_STATUS" = "200" ]; then
        ok "Landing page: HTTP $LANDING_STATUS"
    else
        echo "    WARNING: Landing page returned HTTP $LANDING_STATUS"
    fi
    if [ "$VIEWER_STATUS" = "200" ]; then
        ok "Viewer page: HTTP $VIEWER_STATUS"
    else
        echo "    WARNING: Viewer page returned HTTP $VIEWER_STATUS"
    fi

    # Check for startup errors in logs
    ERRORS=$(remote "sudo journalctl -u tmtv-server --since '2 min ago' --no-pager -p err" 2>/dev/null || echo "")
    if [ -n "$ERRORS" ] && [ "$ERRORS" != "-- No entries --" ]; then
        echo "    WARNING: Errors in server logs since deploy:"
        echo "$ERRORS" | head -10
    else
        ok "No errors in recent server logs"
    fi
fi

# --- Step 6: Integration tests ---

if [ "$SKIP_TESTS" = "true" ]; then
    info "Skipping tests (--skip-tests)"
elif [ "$MODE" = "binary-only" ] || [ "$MODE" = "web-only" ]; then
    info "Skipping tests (deploy mode: $MODE)"
else
    step "Running integration tests on staging"

    # Upload latest test scripts
    scp $SCP_OPTS "$REPO_ROOT/tests/test-integration.sh" "${STAGING_TARGET}:/tmp/test-integration.sh" || {
        err "Failed to upload test script"
        exit 5
    }
    scp $SCP_OPTS "$REPO_ROOT/tests/test-password-prompt.js" "${STAGING_TARGET}:/tmp/test-password-prompt.js" || {
        err "Failed to upload test-password-prompt.js"
        exit 5
    }
    scp $SCP_OPTS "$REPO_ROOT/tests/test-ttl-viewer.js" "${STAGING_TARGET}:/tmp/test-ttl-viewer.js" || {
        err "Failed to upload test-ttl-viewer.js"
        exit 5
    }
    scp $SCP_OPTS "$REPO_ROOT/tests/test-ctrl-b.js" "${STAGING_TARGET}:/tmp/test-ctrl-b.js" || {
        err "Failed to upload test-ctrl-b.js"
        exit 5
    }

    # Build test command
    TEST_FLAGS="--local"
    if [ "$FULL_TESTS" = "false" ]; then
        TEST_FLAGS="$TEST_FLAGS --quick"
    fi

    # Run tests.
    # TEST_HOST=localhost — test runner runs on the server itself, uses local SSH/SSE ports.
    # WEB_HOST=staging.tmtv.se — Caddy TLS certs are valid for this domain (resolves to
    # 127.0.0.1 on the staging box), so HTTPS web tests work correctly.
    if remote "TEST_HOST=localhost WEB_HOST=${STAGING_HOST} WEB_PROTO=https WEB_PORT=443 sh /tmp/test-integration.sh $TEST_FLAGS"; then
        ok "Integration tests passed"
    else
        err "Integration tests FAILED"
        exit 5
    fi
fi

# --- Summary ---

step "Deploy complete"
if [ "$MODE" != "test-only" ]; then
    info "Server: $(remote 'tmtv-server -V 2>&1' || echo 'unknown')"
    info "Health: $(remote 'curl -s -m 3 http://127.0.0.1:4002/healthz' 2>/dev/null || echo 'unavailable')"
fi
echo ""
