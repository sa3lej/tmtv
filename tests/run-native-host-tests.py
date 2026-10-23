#!/usr/bin/env python3
"""Run production-host regressions and retain evidence for the actual platform.

Build the host first. This does not install packages, contact a relay or emulate
another OS; macOS claims require running this on macOS itself.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import signal
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--expected-arch", choices=("arm64", "x86_64"))
    args = parser.parse_args()
    artifacts = args.artifacts.resolve()
    if artifacts == ROOT or ROOT in artifacts.parents:
        parser.error("keep validation evidence outside the checkout")
    machine = platform.machine()
    if args.expected_arch and machine != args.expected_arch:
        parser.error(f"expected native {args.expected_arch}, got {machine}")
    binary = ROOT / "tmtv"
    if not binary.is_file():
        parser.error("build tmtv before running host regressions")
    artifacts.mkdir(parents=True, exist_ok=True)
    evidence = {
        "platform": platform.platform(), "system": platform.system(),
        "machine": machine, "python": sys.version,
        "commit": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip(),
        "workingTree": subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=ROOT, text=True),
        "binarySha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "coverage": "local production host; no relay, browser or Safari claim",
    }
    # Darwin's default /var/folders TMPDIR can exceed Unix socket path limits.
    # Keep fixtures short and prevent a user's normal config enabling sharing.
    with tempfile.TemporaryDirectory(prefix="tmtv-host-", dir="/tmp") as temp:
        config = Path(temp) / "config" / "tmtv"
        config.mkdir(parents=True)
        (config / "tmtv.conf").write_text('set -s tmtv-server-host ""\n')
        env = dict(os.environ, TMPDIR=temp, XDG_CONFIG_HOME=str(config.parent),
                   TMTV=str(binary))
        for key in ("TMUX", "TMUX_PROGRAM"):
            env.pop(key, None)
        started = time.monotonic()
        command = ["make", "-C", "tests", "host", f"TMTV={binary}",
                   f"PYTHON={sys.executable}"]
        evidence["command"] = command
        try:
            with (artifacts / "host-tests.log").open("w") as log:
                process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=log,
                                           stderr=subprocess.STDOUT,
                                           start_new_session=True)
                try:
                    evidence["exitCode"] = process.wait(timeout=900)
                except subprocess.TimeoutExpired:
                    os.killpg(process.pid, signal.SIGTERM)
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        os.killpg(process.pid, signal.SIGKILL)
                        process.wait()
                    evidence["exitCode"] = 124
                    evidence["error"] = "host regression timeout"
        finally:
            evidence["elapsedSeconds"] = round(time.monotonic() - started, 3)
            evidence["logSha256"] = hashlib.sha256(
                (artifacts / "host-tests.log").read_bytes()).hexdigest()
            (artifacts / "verification.json").write_text(
                json.dumps(evidence, indent=2) + "\n")
    print(json.dumps(evidence, indent=2))
    return evidence["exitCode"]


if __name__ == "__main__":
    sys.exit(main())
