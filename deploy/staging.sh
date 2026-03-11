#!/bin/sh
#
# Deploy tmtv to staging server.
#
# Builds locally (native, incremental), uploads binaries + web assets,
# restarts tmtv-server. Designed to be run many times during development.
#
# Usage:
#   deploy/staging.sh                  # build + deploy all
#   deploy/staging.sh --skip-build     # deploy existing binaries only
#   deploy/staging.sh --web-only       # deploy web assets only
#   deploy/staging.sh --test           # build + deploy + run integration tests
#
# Environment:
#   STAGING_HOST     - staging server (default: staging.tmtv.se)
#   STAGING_USER     - SSH user (default: ubuntu)
#   STAGING_SSH_PORT - SSH port (default: 22)
#

set -e

STAGING_HOST="${STAGING_HOST:-staging.tmtv.se}"
STAGING_USER="${STAGING_USER:-ubuntu}"
STAGING_SSH_PORT="${STAGING_SSH_PORT:-22}"
STAGING_TARGET="${STAGING_USER}@${STAGING_HOST}"

SSH_OPTS="-o StrictHostKeyChecking=accept-new -p ${STAGING_SSH_PORT}"
SCP_OPTS="-o StrictHostKeyChecking=accept-new -P ${STAGING_SSH_PORT}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SKIP_BUILD=false
WEB_ONLY=false
RUN_TESTS=false

for arg in "$@"; do
	case "$arg" in
		--skip-build) SKIP_BUILD=true ;;
		--web-only)   WEB_ONLY=true ;;
		--test)       RUN_TESTS=true ;;
	esac
done

remote() {
	ssh $SSH_OPTS "$STAGING_TARGET" "$@"
}

# --- Build ---
if [ "$SKIP_BUILD" = "false" ] && [ "$WEB_ONLY" = "false" ]; then
	echo "==> Building tmtv..."
	cd "$REPO_ROOT"
	if [ ! -f configure ]; then
		sh autogen.sh
	fi
	if [ ! -f Makefile ]; then
		./configure
	fi
	make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
	echo "    tmtv:        $(./tmtv -V 2>&1)"
	echo "    tmtv-server: $(./tmtv-server -V 2>&1)"
fi

# --- Deploy binaries ---
if [ "$WEB_ONLY" = "false" ]; then
	echo "==> Deploying binaries..."
	scp $SCP_OPTS "$REPO_ROOT/tmtv" "${STAGING_TARGET}:/tmp/tmtv-bin"
	scp $SCP_OPTS "$REPO_ROOT/tmtv-server" "${STAGING_TARGET}:/tmp/tmtv-server-bin"
	remote "sudo mv -f /tmp/tmtv-bin /usr/local/bin/tmtv && \
		sudo mv -f /tmp/tmtv-server-bin /usr/local/bin/tmtv-server && \
		sudo chmod +x /usr/local/bin/tmtv /usr/local/bin/tmtv-server && \
		sudo systemctl restart tmtv-server"
	echo "    Binaries deployed, tmtv-server restarted."
fi

# --- Deploy web assets ---
echo "==> Deploying web assets..."
cd "$REPO_ROOT/site"
if [ ! -d node_modules ]; then
	npm ci
fi
npm run build --silent

scp $SCP_OPTS "$REPO_ROOT/site/dist/index.html" "${STAGING_TARGET}:/tmp/index.html"
scp $SCP_OPTS "$REPO_ROOT/site/dist/viewer/index.html" "${STAGING_TARGET}:/tmp/viewer.html"
scp $SCP_OPTS "$REPO_ROOT/web/viewer.js" "${STAGING_TARGET}:/tmp/viewer.js"
for f in "$REPO_ROOT"/site/dist/*.txt "$REPO_ROOT"/site/dist/*.png "$REPO_ROOT"/site/dist/*.gif "$REPO_ROOT"/site/dist/favicon.*; do
	[ -f "$f" ] && scp $SCP_OPTS "$f" "${STAGING_TARGET}:/tmp/" || true
done
remote "sudo mkdir -p /var/www/tmtv && \
	sudo mv -f /tmp/index.html /var/www/tmtv/index.html && \
	sudo mv -f /tmp/viewer.html /var/www/tmtv/viewer.html && \
	sudo mv -f /tmp/viewer.js /var/www/tmtv/viewer.js && \
	for f in /tmp/*.txt /tmp/*.png /tmp/*.gif /tmp/favicon.*; do \
		[ -f \"\$f\" ] && sudo mv -f \"\$f\" /var/www/tmtv/ || true; \
	done"
echo "    Web assets deployed."

# --- Run tests ---
if [ "$RUN_TESTS" = "true" ]; then
	echo "==> Uploading and running integration tests on staging..."
	scp $SCP_OPTS "$REPO_ROOT/tests/test-integration.sh" "${STAGING_TARGET}:/tmp/test-integration.sh"
	remote "WEB_PROTO=https WEB_PORT=443 TEST_HOST=${STAGING_HOST} sh /tmp/test-integration.sh --local --quick"
fi

echo "==> Done."
