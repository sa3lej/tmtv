#!/usr/bin/env python3
"""Exercise the production history loader through an isolated host server."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
BINARY = Path(os.environ.get("TMTV", ROOT / "tmtv")).resolve()
TYPES = ("command", "search", "target", "window-target")


class PromptHistoryTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(prefix="history-", dir=ROOT / "tests")
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.socket = self.root / "socket"
        self.history = self.root / "history"
        self.config = self.root / "config"
        self.config.write_text(f'set -g history-file "{self.history}"\n')

    def command(self, *args, check=True):
        return subprocess.run(
            [str(BINARY), "-S", str(self.socket), "-f", str(self.config), *args],
            capture_output=True, text=True, timeout=10, check=check,
        )

    def load(self, contents):
        if contents is not None:
            self.history.write_bytes(contents)
        self.addCleanup(self.command, "kill-server", check=False)
        self.command("new-session", "-d", "-s", "history", "sleep 60")
        # This query runs after cfg_done has loaded history on the real server.
        self.command("show-options", "-g", "history-file")

    def entries(self, kind):
        output = self.command("show-prompt-history", "-T", kind).stdout
        self.assertTrue(output.startswith(f"History for {kind}:\n"), output)
        return [line.split(": ", 1)[1] for line in output.splitlines()
                if line.partition(": ")[0].isdigit()]

    def test_typed_legacy_and_unterminated_lines(self):
        self.load(b"command:display-message one\nsearch:needle\ntarget:session:2\n"
                  b"window-target:3\nlegacy without type\nunknown:keep:colons\n"
                  b"\ncommand:final without newline")
        self.assertEqual(self.entries("command"), ["display-message one",
                         "legacy without type", "unknown:keep:colons", "",
                         "final without newline"])
        self.assertEqual(self.entries("search"), ["needle"])
        self.assertEqual(self.entries("target"), ["session:2"])
        self.assertEqual(self.entries("window-target"), ["3"])

    def test_large_line_and_buffer_reuse(self):
        long_line = "x" * 65536
        self.load((f"command:{long_line}\nsearch:short\ncommand:last").encode())
        self.assertEqual(self.entries("command"), [long_line, "last"])
        self.assertEqual(self.entries("search"), ["short"])

    def test_empty_file(self):
        self.load(b"")
        for kind in TYPES:
            self.assertEqual(self.entries(kind), [])

    def test_missing_file(self):
        self.load(None)
        for kind in TYPES:
            self.assertEqual(self.entries(kind), [])


if __name__ == "__main__":
    unittest.main()
