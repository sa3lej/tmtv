#!/usr/bin/env python3
"""Compare committed tmtv integration with pinned official tmux blobs.

Read-only and offline: fetch the two commits with --no-tags before running.
The report describes committed trees, never uncommitted files or build output.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess
import sys


BASE = "cc117b5048f77a4842820f8ebbe3a86e5c077224"  # official 3.6a
TARGET = "7f2a35ad3321f9ba57a1062ca73b1f3ff26aca53"  # official 3.8
REPO = Path(__file__).resolve().parent.parent


def git(*args):
    return subprocess.check_output(["git", "-C", str(REPO), *args])


def tree(commit):
    # Include modes and object types: a mode change must not appear unchanged.
    result = {}
    for entry in git("ls-tree", "-r", "-z", commit).split(b"\0"):
        if entry:
            metadata, path = entry.split(b"\t", 1)
            result[path.decode("utf-8")] = metadata.decode("ascii")
    return result


def component(path):
    if path.startswith("server/"):
        return "relay"
    if path.startswith("tmate-") or path == "tmate.h":
        return "host-sharing"
    if path.startswith("web/"):
        return "web-viewer"
    if path.startswith("tests/"):
        return "tests"
    return "other"


def is_core(path):
    return (
        ("/" not in path and path.endswith((".c", ".h")))
        or path.startswith(("compat/", "regress/", "fuzz/"))
        or path in {"Makefile.am", "configure.ac", "autogen.sh", "mdoc2man.awk"}
    )


def inventory(fork):
    resolved = git("rev-parse", "--verify", fork + "^{commit}").decode().strip()
    base, target, local = tree(BASE), tree(TARGET), tree(resolved)
    shared = []
    integration = []
    # This is deliberately a three-tree comparison, not merge-base: tmux's
    # portable and OpenBSD histories have a much older common ancestor.
    for path in sorted(base.keys() | target.keys() | local.keys()):
        b, t, f = base.get(path), target.get(path), local.get(path)
        if is_core(path) and (b is not None or t is not None):
            if b == f and b == t:
                continue
            if f == t:
                kind = "already-matches-target"
            elif b != f and b != t:
                kind = "overlap-review"
            elif b != f:
                kind = "fork-only-change"
            else:
                kind = "upstream-only-change"
            shared.append({
                "path": path,
                "kind": kind,
                "base": b,
                "fork": f,
                "target": t,
            })
        elif b != f and component(path) != "other":
            integration.append({"path": path, "component": component(path)})
    return {
        "base": {"version": "3.6a", "commit": BASE},
        "target": {"version": "3.8", "commit": TARGET},
        "fork": resolved,
        "counts": dict(sorted(Counter(row["kind"] for row in shared).items())),
        "integration_counts": dict(sorted(Counter(
            row["component"] for row in integration
        ).items())),
        "core": shared,
        "integration": integration,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fork", default="HEAD", help="committed tmtv revision")
    args = parser.parse_args()
    try:
        report = inventory(args.fork)
    except subprocess.CalledProcessError:
        print("Inventory requires a valid fork revision and both pinned commits. "
              "Fetch missing commits from https://github.com/tmux/tmux.git "
              "with git fetch --no-tags <url> <commit>.", file=sys.stderr)
        return 1
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
