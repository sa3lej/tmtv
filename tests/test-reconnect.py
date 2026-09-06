#!/usr/bin/env python3
"""Real reconnect regression tests against an ISOLATED localhost relay.

Requires sudo, ssh-keygen, curl, ss and a built client. Never use a production
relay: this test terminates its own host daemons and drops its TCP connections.
Example: python3 tests/test-reconnect.py --relay-pid PID --keys /tmp/test-keys
    --port 2244 --sse-port 4044
"""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import time


def run(*args, check=True):
    return subprocess.run(args, check=check, text=True, capture_output=True).stdout.strip()


def eventually(fn, timeout=20):
    until = time.monotonic() + timeout
    while time.monotonic() < until:
        result = fn()
        if result:
            return result
        time.sleep(.2)
    raise AssertionError("timed out waiting for reconnect")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--relay-pid", required=True, type=int)
    ap.add_argument("--keys", required=True, type=Path)
    ap.add_argument("--port", required=True, type=int)
    ap.add_argument("--sse-port", required=True, type=int)
    ap.add_argument("--client", default="./tmtv")
    a = ap.parse_args()
    relay_cmd = run("ps", "-p", str(a.relay_pid), "-o", "args=")
    assert "tmtv-server" in relay_cmd and f"-p {a.port}" in relay_cmd
    assert "-b 127.0.0.1" in relay_cmd, "test relay MUST bind localhost"
    client = str(Path(a.client).resolve())
    fp = {}
    for kind in ("rsa", "ed25519"):
        fp[kind] = run("ssh-keygen", "-lf", str(a.keys / f"ssh_host_{kind}_key.pub"),
                       "-E", "sha256").split()[1]
    sockets = []
    paused = []
    with tempfile.TemporaryDirectory(prefix="tmtv-reconnect-") as tmp:
        def start(label, name=""):
            sock = str(Path(tmp) / f"{label}.sock")
            config = Path(tmp) / f"{label}.conf"
            config.write_text(
                f'set -g tmtv-server-host "127.0.0.1"\n'
                f'set -g tmtv-server-port {a.port}\n'
                f'set -g tmtv-server-rsa-fingerprint "{fp["rsa"]}"\n'
                f'set -g tmtv-server-ed25519-fingerprint "{fp["ed25519"]}"\n'
                f'set -g tmtv-session-name "{name}"\n'
                'set -g tmtv-web-sharing on\n')
            sockets.append(sock)
            run(client, "-S", sock, "-f", str(config), "new-session", "-d", "sleep 300")
            eventually(lambda: info(sock)[0])
            return sock

        def info(sock):
            return run(client, "-S", sock, "display", "-p",
                       "#{tmtv_ssh}|#{tmtv_ssh_ro}|#{tmtv_web}|#{tmtv_session_name}").split("|")

        def token(sock):
            return info(sock)[0].split()[-1].split("@")[0]

        def target(sock):
            return run("sudo", "-n", "readlink", f"/tmp/tmtv/sessions/{token(sock)}", check=False)

        def daemon(sock):
            prefix = target(sock)[:4]
            rows = run("ps", "--ppid", str(a.relay_pid), "-o", "pid=,args=")
            matches = [int(row.split()[0]) for row in rows.splitlines()
                       if f"[{prefix}...] (daemon)" in row]
            assert len(matches) == 1, rows
            return matches[0]

        def sse(tok):
            out = run("curl", "-sN", "--max-time", "1", "-D", "-",
                      f"http://127.0.0.1:{a.sse_port}/ws/{tok}", check=False)
            assert out.startswith("HTTP/1.1 200"), out[:200]

        def reconnect(sock):
            before, old = info(sock), target(sock)
            run("sudo", "-n", "kill", "-TERM", str(daemon(sock)))
            eventually(lambda: target(sock) not in ("", old))
            assert info(sock) == before, (before, info(sock))
            sse(token(sock))

        try:
            named = start("named", "reconnect-regression")
            assert re.fullmatch(r"[0-9a-f]{32}-reconnect-regression", token(named))
            for _ in range(3):
                reconnect(named)
            print("PASS named links survive three daemon replacements", flush=True)

            # Pause an old owner, destroy only its SSH transport, then let its
            # replacement claim the aliases before old-owner cleanup runs.
            before, old = info(named), target(named)
            pid = daemon(named)
            run("sudo", "-n", "kill", "-STOP", str(pid))
            paused.append(pid)
            run("sudo", "-n", "ss", "-K", f"sport = :{a.port}")
            eventually(lambda: target(named) not in ("", old))
            replacement = target(named)
            assert info(named) == before
            run("sudo", "-n", "kill", "-CONT", str(pid))
            paused.remove(pid)
            eventually(lambda: not Path(f"/proc/{pid}").exists())
            assert target(named) == replacement
            sse(token(named))
            print("PASS overlapping handover survives old-owner cleanup", flush=True)

            collision = start("collision", "reconnect-regression")
            assert info(collision)[3] == "reconnect-regression-1"
            run(client, "-S", named, "kill-server")
            reconnect(collision)
            assert info(collision)[3] == "reconnect-regression-1"
            print("PASS assigned suffix is remembered even when base name becomes free", flush=True)

            unnamed = start("unnamed")
            original = info(unnamed)
            assert re.fullmatch(r"[0-9a-f]{32}", token(unnamed))
            reconnect(unnamed)
            sse(original[1].split()[-1].split("@")[0])
            run(client, "-S", unnamed, "kill-server")
            unnamed = start("unnamed")
            assert info(unnamed)[0] != original[0]
            assert info(unnamed)[1] != original[1]
            print("PASS unnamed reconnect preserves RW/RO; full restart rotates both", flush=True)

            longname = start("long", "n" * 32)
            assert len(token(longname)) == 65
            reconnect(longname)
            print("PASS maximum-length named token routes through SSH aliases and SSE", flush=True)
        finally:
            for pid in paused:
                run("sudo", "-n", "kill", "-CONT", str(pid), check=False)
            for sock in sockets:
                run(client, "-S", sock, "kill-server", check=False)


if __name__ == "__main__":
    main()
